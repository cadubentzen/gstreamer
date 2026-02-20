# Seek in Ready

## The Problem: Double Preroll on Stack Changes

NLE (Non-Linear Engine) manages a dynamic pipeline stack: the set of sources
and effects active at the current playback position. When a seek or natural
playback causes clips to enter or leave the composition window, NLE tears down
the old stack and builds a new one.

When elements in the new stack transition to PAUSED, they begin prerolling —
processing data starting from position 0. But the desired playback position is
rarely 0. So once the toplevel seek event arrives and propagates through the
pipeline, every element gets flushed and must preroll a second time, from the
correct position.

This double preroll is wasteful, and its cost grows with the complexity of the
stack — especially when nested timelines are involved.

## How Seek in Ready Works

The solution is to seek sources and effects to the correct position while they
are still in READY state, before they start flowing data. That way, when they
transition to PAUSED, they preroll exactly once, already at the right position.

Each NLE stack carries a `can_seek_in_ready` flag, computed in `get_stack_list()`
as a logical AND across every source and effect in the stack. Each NleObject
exposes a `can-seek-in-ready` signal that defaults to TRUE. Subclasses can
override the default, and external code (such as GES) can connect to the signal
to opt out.

When `can_seek_in_ready` is true for the whole stack, `_relink_new_stack()`
sends the seek event to each element during relinking, before any state change.
When it is false, the stack falls back to the normal double-preroll path, which
works correctly but less efficiently.

## Source and Effect Types

### In-memory and test sources

Sources backed by in-memory data or synthetic generators (e.g. `videotestsrc`,
`audiotestsrc`) trivially support seek in ready: they have no I/O to perform and
can position themselves instantly. More generally, any source based on
`GstBaseSrc` supports seek in ready natively, as the base class handles the seek
event in READY state out of the box.

### File and stream sources: `uridecodepoolsrc`

For sources backed by files or network streams, seek in ready is supported via
`uridecodepoolsrc`. This element maintains a pool of pre-opened decoding
pipelines, one per URI. When a clip is about to enter the NLE stack, the
pipeline for that URI is already open and idle, having already prerolled
internally. When NLE sends a seek in READY to this source, the pool pipeline
re-seeks itself internally. From NLE's perspective, the first buffer that comes
out will be at the requested position, without any additional preroll cost at the
NLE level.

### Effects: the compositor and aggregator

Effects based on `GstAggregator` (such as the compositor) also need to receive
the seek event while in READY state. This is because the aggregator uses the
seqnum from the seek event to stamp its output EOS. Without this, the aggregator
would send an EOS with the wrong seqnum, which NLE would misinterpret as a
stack-advance signal rather than a true end-of-stream.

### Nested timelines

A nested timeline (a `GESTimeline` used as the source of a clip) contains an
NleComposition. For the outer stack to have `can_seek_in_ready` true, the inner
composition must also support it — which requires all sources and effects inside
it to support it as well. This is not currently detected across subtimeline
boundaries (see Limitations).

When a nested composition does support seek in ready, it faces an additional
challenge: it must know **which window of its own timeline** the outer
composition is going to use, so that it can seek its own sources to the right
position. This is the initialization seek problem, described in the next section.

## Initialization Seek for Nested Timelines

When an NleComposition initializes its stack, it needs to know its playback
window — the range `[start, stop]` it must prepare. For a top-level composition
this is known directly. For a nested composition, the window is determined by
the outer clip's `inpoint` and `duration`, possibly further clamped by the outer
composition's own stack window, and possibly adjusted by time effects.

### The Query Mechanism

Rather than passing this information top-down through function calls (which
would require tight coupling across layers), NLE uses a bottom-up message query.
A `GST_QUERY_TYPE_ANCESTORS` direction was considered for this purpose but a
`GstMessage` was preferred because nested compositions can run inside auxiliary
pipelines (such as `uridecodepoolsrc`'s pool pipelines) that are isolated from
the main pipeline hierarchy. A GstMessage posted via `gst_element_post_message()`
propagates upward through each bin's `handle_message()` virtual method, which
works across these pipeline boundaries.

The flow is:

1. The inner NleComposition posts a `nleobject-query-initialization-seek`
   message via `gst_element_post_message()`.
2. The message propagates upward through the bin hierarchy via each bin's
   `handle_message()` virtual method.
3. Each ancestor in the chain gets a chance to fill in or refine the seek event
   carried in the message before forwarding it further up.
4. When `post_message()` returns, the inner composition reads the seek event
   that ancestors have placed in the message.

Each level in the hierarchy translates the seek into its own coordinate space:

- **NleSource** (`nle_source_handle_message()`): lets parent bins answer first,
  then translates the resulting seek from composition time to media time using
  the clip's `inpoint` and `start`: `media_time = composition_time - start + inpoint`.
- **Parent NleComposition** (`nle_composition_handle_message()`): provides its
  own `stack_initialization_seek` if none has been set yet, then clamps the
  window to its stack playback window:
  `start = MAX(start, stack_window_start)`, `stop = MIN(stop, stack_window_stop)`.

### Example: Deeply Nested Timeline

The diagram below shows the GstBin containment hierarchy (not the class
hierarchy) for a timeline containing a nested clip:

```
GstPipeline
  └─ GESTimeline (outer)
        └─ NleComposition (outer, track)
            └─ NleSource (wrapping gesdemux → inner GESTimeline)
                 └─ GESTimeline (inner)
                      └─ NleComposition (inner, track)
                           └─ NleSource (leaf source)
```

Message flow when the inner NleComposition initializes:

```
NleComposition(inner) posts "nleobject-query-initialization-seek"
  → GESTimeline(inner).handle_message: no parent_source → forward
    → NleSource(outer).handle_message: lets parent answer first
      → NleComposition(outer).handle_message: provides stack_init_seek, clamps
      ← returns
    ← NleSource translates to media coordinates
  ← returns
← inner composition gets seek = [clamped_start_in_media_time, clamped_stop_in_media_time]
```

## The Preloading Pipeline Edge Case

`uridecodepoolsrc` maintains a pool of pre-opened decoding pipelines. When a
`GESTimeline` is used as the source of a clip, `uridecodepoolsrc` opens it
inside one of these pool pipelines — a self-contained GstPipeline that exists
separately from the main editing pipeline. This pool pipeline has no outer
NleComposition or NleSource in its hierarchy.

This means that when the inner NleComposition posts its initialization seek
query, the message propagates upward but finds no NLE ancestor to answer it.
Without an answer, the composition falls back to initializing with
`stop = full_inner_timeline_duration`, causing the entire inner timeline to be
prerolled unnecessarily.

The fix: `GESTimeline` intercepts the message in `ges_timeline_handle_message()`.
When the timeline knows it is being used as a nested clip source (i.e. it has
a reference to its parent clip), it provides the initialization seek directly
using the parent clip's `inpoint` and `duration`:

```
NleComposition(inner) posts "nleobject-query-initialization-seek"
  → GESTimeline(inner).handle_message: knows its parent clip!
    → parent_class->handle_message: forwards up (no NLE ancestors to answer)
    ← returns (no seek filled in yet)
    → GESTimeline provides seek = [parent_inpoint, parent_inpoint + parent_duration]
  ← returns
← inner composition gets seek = [inpoint, inpoint + duration]
```

## Limitations

The `can_seek_in_ready` flag is computed by ANDing across the sources and
effects in the **current flat stack**. It does not recurse into subtimelines:
the inner composition is treated as a single opaque element. This means that if
a source deep inside a nested timeline does not support seek in ready, the outer
composition will not detect this and will incorrectly proceed as if the whole
stack is seek-in-ready capable.

The result is not a graceful fallback — it will likely cause a stall or other
misbehavior. For seek in ready to work correctly with nested timelines, every
source and effect at every level of nesting must support it.
