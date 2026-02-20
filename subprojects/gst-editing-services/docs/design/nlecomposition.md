# NleComposition Design

## 1. What NleComposition Is

NleComposition is the central element of GStreamer's Non-Linear Engine (NLE)
plugin, which underpins the GES (GStreamer Editing Services) library. It is the
runtime engine that turns an abstract timeline description — a set of clips
placed at positions in time — into a live GStreamer pipeline that produces a
continuous media stream.

In the GESTimeline hierarchy, each track in the timeline is backed by one
top-level NleComposition. Nested timelines (sub-timelines) are represented by
additional NleComposition instances added directly as children of a parent
NleComposition, or embedded inside an NleSource wrapping another GESTimeline —
both arrangements produce equivalent behaviour. The result is a recursive
structure that can reach arbitrary depth.

NleComposition itself is a GstBin and also an NleObject, so it can be placed as
a child inside a parent composition just like any other source or operation. It
exposes a single always-available source pad that carries the output stream.

## 2. Core Concepts

### 2.1 The NleObject Time Model

Every object in the NLE hierarchy (NleSource, NleOperation, NleComposition) is
an NleObject, which attaches four time coordinates to the standard GstBin:

- `start`: the position inside the parent composition at which this object starts
  being used.
- `duration` / `stop`: the length and end point of the object in composition time.
- `inpoint`: the offset into the object's internal media at which to start
  reading, allowing a clip to be trimmed without modifying the source media.
- `priority`: a non-negative integer that controls the rendering order within a
  stack (see [Section 3](#3-stack-management)). Lower values represent higher
  priority (priority 0 is the topmost layer).

An object is considered active if its duration is greater than zero and its
`active` flag is set. An `expandable` object is a special case whose start and
stop are automatically stretched to match the full composition duration; this is
used for background tracks.

### 2.2 Priority and Compositing

The `priority` field determines how overlapping objects are assembled into a
rendering stack. Its primary use case is video compositing: higher-priority
objects (lower `priority` value) are placed on top of lower-priority ones,
typically by feeding them as foreground inputs to a blending or compositing
operation. For example, a title overlay at priority 0 is composited on top of a
background video at priority 1.

For audio, strict layering is less intuitive, but priority still governs the
order in which sources are connected to mixing operations and which source is
treated as the primary stream when no mixer is present.

See [Section 3.1](#31-what-the-stack-is) for how priority translates into the
source/operation tree that forms the active sub-pipeline.

### 2.3 Time Coordinate Translation

NleObject pads perform time coordinate translation on all events that make sense
(seek/segments). On the source pad, outgoing times are shifted by `+start - inpoint`,
mapping from the object's internal source coordinates to the parent
composition's timeline coordinates. On the sink pad (for operations), incoming
times are shifted by `-start + inpoint`.

When two NleObjects are linked — an upstream source feeding a downstream
operation — the combined shift translates from the upstream object's internal
source coordinates to the downstream object's internal sink coordinates. This
allows the GStreamer segment mechanism to work correctly across the stack without
any explicit clock-time translation in the media pipeline itself.

For compositions that contain time effects (elements that alter the media
consumption rate), see `time_notes.md` for the constraints those effects must
satisfy and the mechanism used to compensate for the resulting coordinate
distortion.

### 2.4 The Composition Window

At any given composition position, only a subset of NleObjects are active and
wired together into what is called a *stack* (described in detail in
[Section 3](#3-stack-management)). The composition tracks two related intervals
that define when the current stack is valid:

- `stack_start` / `stack_stop`: the natural boundaries of the currently active
  set of objects.
- `stack_playback_window_start` / `stack_playback_window_stop`: the actual range
  over which the current sub-pipeline is valid without requiring a topology
  change. This is narrower than or equal to the stack's natural boundaries
  because objects at lower priority levels may start or stop within the stack's
  interval.

## 3. Stack Management

### 3.1 What the Stack Is

The "stack" is the set of NleObjects that are active at a given composition
position and the way they are linked together to form the active sub-pipeline.
It is represented as a `GNode` tree:

- Leaf nodes are NleSources (or NleCompositions acting as sources). Each leaf
  provides one stream of media data.
- Interior nodes are NleOperations. An operation accepts one or more input
  streams from its children and produces one output stream, implementing
  blending, transitions, or effects.
- The single root node's source pad is wired to the composition's ghost pad,
  becoming the composition's output.

The composition maintains a `current_bin` (a GstBin) that holds all elements
belonging to the active stack. When the stack changes, elements are removed from
and added to this bin.

### 3.2 Building the Stack

Stack construction collects all NleObjects whose `[start, stop)` interval
contains the query timestamp, sorts them by priority, and converts the flat list
into a tree by consuming NleOperations and assigning them children in priority
order. The playback window stop is then tightened by scanning for any
lower-priority object whose boundary falls within the natural stack boundaries,
ensuring the stack is rebuilt at the precise moment an object enters or leaves.

### 3.3 Stack Transitions

When the stack must change:

1. `current_bin` is transitioned to READY and flushed downstream if needed.
2. All elements are removed from `current_bin`.
3. The new stack is linked inside `current_bin` and each object receives a
   translated initialization seek.
4. The composition's ghost pad target is updated to the new root object's source
   pad and `current_bin` is activated.

If the new stack is identical to the old one, no teardown occurs — the existing
stack is simply re-seeked.

How the initialization seek is delivered differs between the legacy and modern
approaches; see [Section 5.2](#52-initialization-seeks-and-seek-in-ready).

## 4. State Machine and Threading

### 4.1 The Action Task

All pipeline mutations run on a dedicated GstTask
(`<comp-name>_update_management`). Actions are closures in a queue;
high-priority actions are prepended, normal ones appended. This single-threaded
serialization ensures no two mutations race.

Key action types:

- `_initialize_stack_func`: builds and activates the first stack for a new
  playback position.
- `_commit_func`: applies pending property changes and rebuilds the stack
  (see [Section 4.2](#42-the-commit-mechanism)).
- `_update_pipeline_func`: advances to the next stack after EOS or after a
  playback window boundary is crossed.
- `_seek_pipeline_func`: handles a seek event, either re-seeking the current
  stack in place or triggering a full stack rebuild.
- `_add_object_func` / `_remove_object_func`: add or remove child NleObjects
  from the composition.
- `_update_after_eos_while_scrubbing`: deferred stack advance during rapid
  scrubbing (see [Section 6.2](#62-eos-and-gap-handling)).

### 4.2 The Commit Mechanism

NleObject uses a two-phase property update pattern. Setting `start`, `duration`,
`inpoint`, `priority`, or `active` records the change in `pending_*` fields
without applying it immediately. When `nle_object_commit()` is called on the
composition itself, a `_commit_func` action runs: pending children
additions/removals are processed, `_commit_values` applies all `pending_*`
values recursively, sorted lists are re-sorted, and the stack is rebuilt.

### 4.3 Pausing and Restarting the Task (Legacy Seek-After-Preroll)

This is the original, less efficient mechanism for activating a new stack. After
the new elements are linked and brought to PLAYING, the composition sends an
initialization seek and then pauses the action task, recording a
`seqnum_to_restart_task`. The ghost pad probe watches for the matching
FLUSH_STOP or SEGMENT event — confirming that the stack has prerolled and the
seek has been accepted — then restarts the task and optionally emits the
`commited` signal.

This approach works but requires the stack to reach PLAYING before it can be
seeked, which introduces unnecessary latency especially for nested compositions.
See [Section 5.2](#52-initialization-seeks-and-seek-in-ready) for the more
efficient seek-in-ready mechanism that supersedes this for nested cases.

## 5. Seeking

### 5.1 External Seeks

Seek events queue a `_seek_pipeline_func` action (replacing any previously
queued seek). When it runs, if the seek position falls within the current
playback window the current stack is re-seeked in place; otherwise the stack is
rebuilt for the new position.

### 5.2 Initialization Seeks and Seek-in-Ready

When a new stack is activated, each NleObject receives an initialization seek
translated into its local coordinate space
(`nle_object_translate_incoming_seek`). The legacy approach (seek after preroll,
described in [Section 4.3](#43-pausing-and-restarting-the-task-legacy-seek-after-preroll))
requires the stack to reach PLAYING state before the seek can be processed.

The modern approach — seek-in-ready — allows NleCompositions to process the
initialization seek while still in READY state, avoiding the costly
PLAYING→READY→PLAYING round-trip at every stack change. See `seek_in_ready.md`
for the full mechanism.

## 6. Segment Accumulation and EOS Handling

### 6.1 Segment Accumulation

Each time the composition transitions to a new stack it must keep downstream
running time continuous. This is achieved through GStreamer's *segment
accumulation* mechanism: the composition adjusts the `base` field of outgoing
SEGMENT events to account for the running time accumulated by all previous
stacks. Without this, downstream elements would observe a running-time reset at
every stack change, breaking A/V synchronisation.

### 6.2 EOS and Gap Handling

Each object in the stack is configured with a segment window that matches its
active time range. When playback reaches the end of that window, the object
emits EOS. The ghost pad probe intercepts this EOS: if more content follows in
the composition, the EOS is dropped and a `_update_pipeline_func` action
advances to the next stack. If the composition is truly done, the EOS is
forwarded downstream.

NleComposition does not natively support gaps. If `get_stack_list` returns NULL
while within the composition's bounds, a `GST_ELEMENT_ERROR` is posted. The
layer above is responsible for ensuring full coverage, typically via expandable
objects (e.g. background tracks) that always span the full composition duration.
