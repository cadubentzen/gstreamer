#include "ges-pipeline-pool-manager.h"
#include "ges-internal.h"

#include <gst/video/video.h>


#undef GST_CAT_DEFAULT
#define GST_CAT_DEFAULT ges_pipeline_pool_manager_debug

GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

typedef struct {
  GESClip *clip;
  GESTrack *track;
  /* Committed values captured during timeline commit - these reflect
   * what NLE sees, not potentially uncommitted GES edits */
  GstClockTime timeline_inpoint;    /* Where we start reading in the nested timeline */
  GstClockTime timeline_duration;   /* How much of the nested timeline we read */
  GstClockTime outer_start;         /* Where the clip appears in the outer timeline */
} NestedTimelineInfo;

typedef struct
{
  GstElement *element;

  /* Not keeping ref as it is only used to do pointer comparison */
  GESTrack *track;

  GstClockTime start;
  GstClockTime end;

  GObject *decoderpipe;

  /* For nested timeline sources */
  GESTimeline *source_timeline; /* NULL for top-level sources */
} PooledSource;

struct _GESPipelinePoolManager {
    GRecMutex lock;
    GArray *pooled_sources;
    GArray *prepared_sources;
    GESTimeline *timeline;
    GObject *pool;

    gboolean rendering;

    guint max_preloaded_sources;

    GArray *pending_nested_timelines;
    GPtrArray *registered_nested_timelines;
};

static gint
compare_pooled_source (PooledSource * a, PooledSource * b)
{
  if (a->start == b->start) {
    if (a->end < b->end)
      return -1;
    if (a->end > b->end)
      return 1;
    return 0;
  } else if (a->start < b->start) {
    return -1;
  }

  return 1;
}


static void
pooled_source_clear (PooledSource * s)
{
  gst_object_unref (s->element);
}

void
ges_pipeline_pool_manager_prepare_pipelines_around (GESPipelinePoolManager *
    self, GESTrack * track, GstClockTime stack_start, GstClockTime stack_end)
{
  gboolean entered_window = FALSE;
  GstClockTime window_dur = 60 * GST_SECOND;
  GstClockTime window_start;
  GstClockTime window_stop = stack_end + window_dur;
  gint max_preloaded_sources = self->max_preloaded_sources;
  GArray *to_prepare;
  GPtrArray *to_remove;

  if (!GES_IS_VIDEO_TRACK (track)) {
    GST_DEBUG_OBJECT (self->timeline,
        "Not preparing neighboors anything for %s track",
        G_OBJECT_TYPE_NAME (track));

    return;
  }

  if (!GST_CLOCK_TIME_IS_VALID (stack_start)) {
    GST_INFO_OBJECT (track, "Got invalid stack start, not preparing anything.");
    return;
  }

  g_rec_mutex_lock (&self->lock);
  if (!self->pooled_sources) {
    g_rec_mutex_unlock (&self->lock);
    return;
  }

  GstState state, pending;
  gboolean playing = (
      (gst_element_get_state (GST_ELEMENT (track), &state, &pending,
              0) == GST_STATE_CHANGE_SUCCESS) && state == GST_STATE_PLAYING
      && pending == GST_STATE_VOID_PENDING);
  if (self->rendering || playing) {
    GST_DEBUG_OBJECT (self->timeline, "We are %s not loading clips before",
        playing ? "playing" : "rendering");
    window_start = stack_start;
    max_preloaded_sources = max_preloaded_sources / 2;
  } else {
    window_start = stack_start >= window_dur ? stack_start - window_dur : 0;
  }

  GESSource *parent_source = timeline_get_parent_uri_source (self->timeline);
  if (parent_source) {
    /* TODO: time-effect: Add support for time effects! */
    window_start =
        MAX (window_start, GES_TIMELINE_ELEMENT_INPOINT (parent_source));
    window_dur =
        MIN (window_dur, GES_TIMELINE_ELEMENT_DURATION (parent_source));
    GST_DEBUG_OBJECT (self->timeline,
        "Reducing window %" GST_PTR_FORMAT "[%" GST_TIMEP_FORMAT "- %"
        GST_TIMEP_FORMAT "]", parent_source, &window_start, &window_dur);
  }
  g_clear_object (&parent_source);

  GST_ERROR_OBJECT (self->timeline, "Preparing pipelines around %" GST_TIME_FORMAT
      " - %" GST_TIME_FORMAT " window: [%" GST_TIMEP_FORMAT " - %"
      GST_TIMEP_FORMAT "]" " in %d soures", GST_TIME_ARGS (stack_start),
      GST_TIME_ARGS (stack_end), &window_start, &window_stop,
      self->pooled_sources->len);

  to_prepare = g_array_new (FALSE, FALSE, sizeof (PooledSource));
  for (gint i = 0; i < self->pooled_sources->len; i++) {
    PooledSource *source =
        &g_array_index (self->pooled_sources, PooledSource, i);

    if (!entered_window) {
      if (source->start >= window_start) {
        entered_window = TRUE;
      } else {
        continue;
      }
    }

    if (source->start >= stack_start && GST_CLOCK_TIME_IS_VALID (stack_end)
        && source->end <= stack_end) {
      /* Do not reload sources that are currently running */
      continue;
    }

    if (source->start > window_stop) {
      break;
    }

    if (source->track != track) {
      continue;
    }

    PooledSource ps = {
      .element = gst_object_ref (source->element),
      .track = source->track,
      .start = source->start,
      .end = source->end,
    };
    g_array_append_val (to_prepare, ps);

    if (self->prepared_sources->len + to_prepare->len >= max_preloaded_sources) {
      GST_INFO_OBJECT (self->timeline, "%d sources to prepare already.",
          max_preloaded_sources);
      break;
    }
  }

  to_remove = g_ptr_array_sized_new (self->prepared_sources->len);
  g_ptr_array_set_free_func (to_remove, (GDestroyNotify) gst_object_unref);
  for (gint i = 0; i < self->prepared_sources->len; i++) {
    PooledSource *source =
        &g_array_index (self->prepared_sources, PooledSource, i);

    if (source->track != track)
      continue;

    gboolean in_window = (source->start >= window_start)
        && (source->start <= window_stop);
    if (!in_window) {
      GST_DEBUG_OBJECT (self->timeline,
          "Unpreparing pipeline for %s [%" GST_TIMEP_FORMAT "- %"
          GST_TIMEP_FORMAT "]", GST_OBJECT_NAME (source->element),
          &source->start, &source->end);
      g_ptr_array_add (to_remove, gst_object_ref (source->element));
      GST_DEBUG_OBJECT (self->timeline, "Unprepared %s",
          GST_OBJECT_NAME (source->element));
    }
  }

  GST_DEBUG_OBJECT (self->timeline, "%d sources prepared",
      self->prepared_sources->len);
  g_rec_mutex_unlock (&self->lock);

  for (guint i = 0; i < to_prepare->len; i++) {
    PooledSource *source = &g_array_index (to_prepare, PooledSource, i);

    GST_DEBUG_OBJECT (self->timeline,
        "Preparing %s [%" GST_TIMEP_FORMAT "- %" GST_TIMEP_FORMAT "]",
        GST_OBJECT_NAME (source->element), &source->start, &source->end);
    g_signal_emit_by_name (self->pool, "prepare-pipeline", source->element,
        &source->decoderpipe);

    if (source->decoderpipe) {
      g_rec_mutex_lock (&self->lock);
      if (self->prepared_sources) {
        g_array_append_val (self->prepared_sources, *source);
      }
      g_rec_mutex_unlock (&self->lock);
    } else {
      gst_object_unref (source->element);
    }
  }
  g_array_free (to_prepare, TRUE);

#ifndef GST_DISABLE_GST_DEBUG
  g_rec_mutex_lock (&self->lock);
  if (self->prepared_sources &&
      gst_debug_category_get_threshold (GST_CAT_DEFAULT) >= GST_LEVEL_DEBUG) {
    for (gint i = 0; i < self->prepared_sources->len; i++) {
      PooledSource *source =
          &g_array_index (self->prepared_sources, PooledSource, i);
      GST_DEBUG_OBJECT (self->timeline,
          "Prepared: %s [%" GST_TIMEP_FORMAT "- %" GST_TIMEP_FORMAT "]",
          GST_OBJECT_NAME (source->element), &source->start, &source->end);
    }
  }
  g_rec_mutex_unlock (&self->lock);
#endif

  for (guint i = 0; i < to_remove->len; i++) {
    GstElement *element = g_ptr_array_index (to_remove, i);
    gboolean res;
    g_signal_emit_by_name (self->pool, "unprepare-pipeline", element, &res);
    GST_LOG_OBJECT (self->timeline, "Unprepared %s: result %s",
        GST_OBJECT_NAME (element), res ? "TRUE" : "FALSE");
  }

  g_ptr_array_unref (to_remove);
}

void
ges_pipeline_pool_manager_unprepare_all (GESPipelinePoolManager * self)
{
  GPtrArray *to_unprepare;

  g_rec_mutex_lock (&self->lock);
  if (!self->prepared_sources) {
    g_rec_mutex_unlock (&self->lock);
    return;
  }

  to_unprepare = g_ptr_array_sized_new (self->prepared_sources->len);
  g_ptr_array_set_free_func (to_unprepare, (GDestroyNotify) gst_object_unref);
  for (gint i = 0; i < self->prepared_sources->len; i++) {
    PooledSource *source =
        &g_array_index (self->prepared_sources, PooledSource, i);
    g_ptr_array_add (to_unprepare, gst_object_ref (source->element));
  }
  g_rec_mutex_unlock (&self->lock);

  for (guint i = 0; i < to_unprepare->len; i++) {
    GstElement *element = g_ptr_array_index (to_unprepare, i);
    gboolean res;
    g_signal_emit_by_name (self->pool, "unprepare-pipeline", element, &res);
  }
  g_ptr_array_unref (to_unprepare);
}

static void
nested_timeline_info_clear (NestedTimelineInfo * info)
{
  g_object_unref (info->clip);
}

/* With self->lock taken */
static gboolean
list_pooled_sources (GNode * node, GESPipelinePoolManager * self)
{
  if (GES_IS_AUDIO_URI_SOURCE (node->data)
      || GES_IS_VIDEO_URI_SOURCE (node->data)) {
    gboolean is_nested_timeline;
    GESClip *clip = GES_CLIP (GES_TIMELINE_ELEMENT_PARENT (node->data));

    g_object_get (ges_extractable_get_asset (GES_EXTRACTABLE (clip)),
        "is-nested-timeline", &is_nested_timeline, NULL);

    if (is_nested_timeline) {
      /* Capture committed values now - these are the values NLE sees */
      GstClockTime clip_start = GES_TIMELINE_ELEMENT_START (clip);
      GstClockTime clip_inpoint = GES_TIMELINE_ELEMENT_INPOINT (clip);
      GstClockTime clip_duration = GES_TIMELINE_ELEMENT_DURATION (clip);

      NestedTimelineInfo info = {
        .clip = g_object_ref (clip),
        .track = ges_track_element_get_track (node->data),
        .timeline_inpoint = clip_inpoint,
        .timeline_duration = clip_duration,
        .outer_start = clip_start,
      };

      GST_DEBUG_OBJECT (self->timeline,
          "Recording pending nested timeline clip %" GES_FORMAT
          " inpoint=%" GST_TIME_FORMAT " duration=%" GST_TIME_FORMAT,
          GES_ARGS (clip), GST_TIME_ARGS (clip_inpoint),
          GST_TIME_ARGS (clip_duration));

      g_array_append_val (self->pending_nested_timelines, info);
      return FALSE;
    }

    GstElement *source_element =
        ges_source_get_source_element (GES_SOURCE (node->data));
    if (!g_strcmp0 (GST_OBJECT_NAME (gst_element_get_factory (source_element)),
            "uridecodepoolsrc")) {
      PooledSource s = {
        .element = gst_object_ref (source_element),
        .track = ges_track_element_get_track (node->data),
        .start = GES_TIMELINE_ELEMENT_START (node->data),
        .end = GES_TIMELINE_ELEMENT_END (node->data),
        .source_timeline = NULL,
      };

      g_array_append_val (self->pooled_sources, s);
    }
  }

  g_array_sort (self->pooled_sources, (GCompareFunc) compare_pooled_source);

  return FALSE;
}

void
ges_pipeline_pool_manager_set_rendering (GESPipelinePoolManager * self,
    gboolean rendering)
{
  GST_DEBUG_OBJECT (self->timeline, "Set rendering %d", rendering);

  g_rec_mutex_lock (&self->lock);
  self->rendering = rendering;
  g_rec_mutex_unlock (&self->lock);
}

static void
ges_pipeline_pool_manager_prepare_pipeline_removed_cb (GObject * pool,
    GstElement * src, GObject * decoderpipe, GESPipelinePoolManager * self)
{
  g_rec_mutex_lock (&self->lock);
  if (!self->prepared_sources) {
    g_rec_mutex_unlock (&self->lock);

    return;
  }

  for (gint i = 0; i < self->prepared_sources->len; i++) {
    PooledSource *source =
        &g_array_index (self->prepared_sources, PooledSource, i);
    if (source->decoderpipe == decoderpipe) {
      gst_object_unref (source->decoderpipe);
      GST_DEBUG_OBJECT (self->timeline, "Removing prepared source %s",
          GST_OBJECT_NAME (src));
      g_array_remove_index (self->prepared_sources, i);
      break;
    }
  }
  g_rec_mutex_unlock (&self->lock);
}

typedef struct
{
  GESPipelinePoolManager *pool_manager;
  GESTimeline *nested_timeline;
  NestedTimelineInfo *info;
} NestedTimelineTraversalData;

static gboolean
list_nested_timeline_sources (GNode * node, NestedTimelineTraversalData * data)
{
  GESPipelinePoolManager *self = data->pool_manager;
  NestedTimelineInfo *info = data->info;

  if (GES_IS_AUDIO_URI_SOURCE (node->data)
      || GES_IS_VIDEO_URI_SOURCE (node->data)) {
    gboolean is_nested_timeline;
    GESClip *clip = GES_CLIP (GES_TIMELINE_ELEMENT_PARENT (node->data));

    g_object_get (ges_extractable_get_asset (GES_EXTRACTABLE (clip)),
        "is-nested-timeline", &is_nested_timeline, NULL);

    if (is_nested_timeline) {
      GESTrack *nested_track = ges_track_element_get_track (node->data);

      if (nested_track->type != info->track->type) {
        return FALSE;
      }

      /* Deeply nested timeline - compute coordinates for it */
      GstClockTime clip_start = GES_TIMELINE_ELEMENT_START (clip);
      GstClockTime clip_inpoint = GES_TIMELINE_ELEMENT_INPOINT (clip);
      GstClockTime clip_duration = GES_TIMELINE_ELEMENT_DURATION (clip);
      GstClockTime clip_end = clip_start + clip_duration;

      /* Use parent's visible window in nested timeline coordinates */
      GstClockTime inner_visible_start = info->timeline_inpoint;
      GstClockTime inner_visible_end =
          info->timeline_inpoint + info->timeline_duration;

      /* Check if deeply nested clip intersects the visible window */
      if (clip_end <= inner_visible_start || clip_start >= inner_visible_end) {
        GST_DEBUG_OBJECT (self->timeline,
            "Deeply nested timeline %" GES_FORMAT " outside visible window",
            GES_ARGS (clip));
        return FALSE;
      }

      /* Clamp clip position to visible window */
      GstClockTime clamped_start =
          CLAMP (clip_start, inner_visible_start, inner_visible_end);
      GstClockTime clamped_end =
          CLAMP (clip_end, inner_visible_start, inner_visible_end);

      /* Convert to outer timeline coordinates */
      GstClockTime outer_start =
          clamped_start - info->timeline_inpoint + info->outer_start;

      /* Adjust inpoint based on how much we trimmed from the start */
      GstClockTime adjusted_inpoint =
          clip_inpoint + (clamped_start - clip_start);
      GstClockTime adjusted_duration = clamped_end - clamped_start;

      NestedTimelineInfo nested_info = {
        .clip = g_object_ref (clip),
        .track = info->track,
        .timeline_inpoint = adjusted_inpoint,
        .timeline_duration = adjusted_duration,
        .outer_start = outer_start,
      };

      GST_DEBUG_OBJECT (self->timeline,
          "Recording deeply nested timeline clip %" GES_FORMAT
          " outer_start=%" GST_TIME_FORMAT " inpoint=%" GST_TIME_FORMAT
          " duration=%" GST_TIME_FORMAT,
          GES_ARGS (clip), GST_TIME_ARGS (outer_start),
          GST_TIME_ARGS (adjusted_inpoint), GST_TIME_ARGS (adjusted_duration));

      g_array_append_val (self->pending_nested_timelines, nested_info);
      return FALSE;
    }

    GstElement *source_element =
        ges_source_get_source_element (GES_SOURCE (node->data));
    if (!g_strcmp0 (GST_OBJECT_NAME (gst_element_get_factory (source_element)),
            "uridecodepoolsrc")) {
      GESTrack *nested_track = ges_track_element_get_track (node->data);

      if (nested_track->type != info->track->type) {
        return FALSE;
      }

      /* Source times are in nested timeline coordinates */
      GstClockTime source_start = GES_TIMELINE_ELEMENT_START (node->data);
      GstClockTime source_end = GES_TIMELINE_ELEMENT_END (node->data);

      /* Use committed values captured at commit time */
      GstClockTime inner_visible_start = info->timeline_inpoint;
      GstClockTime inner_visible_end =
          info->timeline_inpoint + info->timeline_duration;

      /* Check if source intersects the visible window (in inner coordinates) */
      if (source_end <= inner_visible_start
          || source_start >= inner_visible_end) {
        return FALSE;
      }

      /* Clamp to visible window in inner coordinates, then convert to outer */
      GstClockTime clamped_start =
          CLAMP (source_start, inner_visible_start, inner_visible_end);
      GstClockTime clamped_end =
          CLAMP (source_end, inner_visible_start, inner_visible_end);

      /* Convert to outer timeline coordinates:
       * outer = inner - timeline_inpoint + outer_start */
      GstClockTime outer_start =
          clamped_start - info->timeline_inpoint + info->outer_start;
      GstClockTime outer_end =
          clamped_end - info->timeline_inpoint + info->outer_start;

      PooledSource s = {
        .element = gst_object_ref (source_element),
        .track = info->track,
        .start = outer_start,
        .end = outer_end,
        .source_timeline = data->nested_timeline,
      };

      GST_DEBUG_OBJECT (self->timeline,
          "Adding nested source %s [%" GST_TIME_FORMAT " - %" GST_TIME_FORMAT
          "] (inner: [%" GST_TIME_FORMAT " - %" GST_TIME_FORMAT "])",
          GST_OBJECT_NAME (source_element),
          GST_TIME_ARGS (s.start), GST_TIME_ARGS (s.end),
          GST_TIME_ARGS (clamped_start), GST_TIME_ARGS (clamped_end));

      g_array_append_val (self->pooled_sources, s);
    }
  }

  return FALSE;
}

/* Add sources from a registered nested timeline using its committed coordinates.
 * Must be called with self->lock held. */
static void
add_nested_timeline_sources (GESPipelinePoolManager * self,
    GESTimeline * nested_timeline)
{
  GESSource *parent_source;
  GESClip *parent_clip;

  if (!self->pooled_sources || !self->pending_nested_timelines)
    return;

  parent_source = timeline_get_parent_uri_source (nested_timeline);
  if (!parent_source) {
    GST_DEBUG_OBJECT (self->timeline,
        "Nested timeline has no parent source, skipping");
    return;
  }

  parent_clip = GES_CLIP (GES_TIMELINE_ELEMENT_PARENT (parent_source));

  GST_DEBUG_OBJECT (self->timeline,
      "Looking for pending entry matching clip %" GES_FORMAT,
      GES_ARGS (parent_clip));

  for (guint i = 0; i < self->pending_nested_timelines->len; i++) {
    NestedTimelineInfo *info =
        &g_array_index (self->pending_nested_timelines, NestedTimelineInfo, i);

    if (info->clip != parent_clip)
      continue;

    GST_DEBUG_OBJECT (self->timeline,
        "Found matching pending entry for clip %" GES_FORMAT,
        GES_ARGS (parent_clip));

    GNode *tree = timeline_get_tree (nested_timeline);
    NestedTimelineTraversalData data = {
      .pool_manager = self,
      .nested_timeline = nested_timeline,
      .info = info,
    };

    g_node_traverse (tree, G_IN_ORDER, G_TRAVERSE_LEAVES, -1,
        (GNodeTraverseFunc) list_nested_timeline_sources, &data);
  }

  g_object_unref (parent_source);

  GST_DEBUG_OBJECT (self->timeline,
      "After adding nested timeline sources, pooled_sources has %d entries",
      self->pooled_sources->len);
}

void
ges_pipeline_pool_manager_register_nested_timeline (GESPipelinePoolManager *
    self, GESTimeline * nested_timeline)
{
  g_rec_mutex_lock (&self->lock);
  g_ptr_array_add (self->registered_nested_timelines, nested_timeline);
  add_nested_timeline_sources (self, nested_timeline);
  g_array_sort (self->pooled_sources, (GCompareFunc) compare_pooled_source);
  g_rec_mutex_unlock (&self->lock);
}

void
ges_pipeline_pool_manager_deregister_nested_timeline (GESPipelinePoolManager *
    self, GESTimeline * nested_timeline)
{
  g_rec_mutex_lock (&self->lock);
  g_ptr_array_remove (self->registered_nested_timelines, nested_timeline);

  if (self->pooled_sources) {
    for (gint i = self->pooled_sources->len - 1; i >= 0; i--) {
      PooledSource *source =
          &g_array_index (self->pooled_sources, PooledSource, i);
      if (source->source_timeline == nested_timeline)
        g_array_remove_index (self->pooled_sources, i);
    }
  }

  if (self->prepared_sources) {
    GPtrArray *to_unprepare = g_ptr_array_new_with_free_func (
        (GDestroyNotify) gst_object_unref);

    for (gint i = self->prepared_sources->len - 1; i >= 0; i--) {
      PooledSource *source =
          &g_array_index (self->prepared_sources, PooledSource, i);
      if (source->source_timeline == nested_timeline) {
        g_ptr_array_add (to_unprepare, gst_object_ref (source->element));
        g_array_remove_index (self->prepared_sources, i);
      }
    }
    g_rec_mutex_unlock (&self->lock);

    for (guint i = 0; i < to_unprepare->len; i++) {
      GstElement *element = g_ptr_array_index (to_unprepare, i);
      gboolean res;
      g_signal_emit_by_name (self->pool, "unprepare-pipeline", element, &res);
    }
    g_ptr_array_unref (to_unprepare);
    return;
  }

  g_rec_mutex_unlock (&self->lock);
}

void
ges_pipeline_pool_manager_commit (GESPipelinePoolManager * self)
{
  g_rec_mutex_lock (&self->lock);
  if (!self->pooled_sources)
    goto done;

  GNode *tree = timeline_get_tree (self->timeline);
  g_array_remove_range (self->pooled_sources, 0, self->pooled_sources->len);
  if (self->pending_nested_timelines)
    g_array_remove_range (self->pending_nested_timelines, 0,
        self->pending_nested_timelines->len);
  g_node_traverse (tree, G_IN_ORDER, G_TRAVERSE_LEAVES, -1,
      (GNodeTraverseFunc) list_pooled_sources, self);

  /* Re-add sources from registered nested timelines using committed coords */
  for (guint i = 0; i < self->registered_nested_timelines->len; i++) {
    GESTimeline *nested =
        g_ptr_array_index (self->registered_nested_timelines, i);
    add_nested_timeline_sources (self, nested);
  }

  g_array_sort (self->pooled_sources, (GCompareFunc) compare_pooled_source);

done:
  g_rec_mutex_unlock (&self->lock);
}

static void
deep_element_added_cb (GstBin * _pipeline, GstBin * _sub_bin,
    GstElement * element)
{
  if (!GST_IS_VIDEO_DECODER (element)) {
    return;
  }

  GST_DEBUG_OBJECT (element, "Output out of segment frames!");
  g_object_set (element, "output-out-of-segment", TRUE, NULL);
}

static void
new_pipeline_cb (GObject * pool, GstElement * pipeline,
    GESPipelinePoolManager * self)
{
  GST_DEBUG_OBJECT (pipeline, "Connecting %" GST_PTR_FORMAT, pool);
  g_signal_connect (pipeline, "deep-element-added",
      G_CALLBACK (deep_element_added_cb), NULL);
}

static void ges_pipeline_pool_manager_deinit (GObject * _pool,
    GESPipelinePoolManager * self);

static void
ges_pipeline_pool_manager_clear (GESPipelinePoolManager * self)
{
  g_rec_mutex_lock (&self->lock);
  if (self->pooled_sources) {
    g_array_free (self->pooled_sources, TRUE);
    g_array_free (self->prepared_sources, TRUE);
    self->prepared_sources = NULL;
    self->pooled_sources = NULL;
  }

  if (self->pending_nested_timelines) {
    g_array_free (self->pending_nested_timelines, TRUE);
    self->pending_nested_timelines = NULL;
  }

  g_clear_pointer (&self->registered_nested_timelines, g_ptr_array_unref);

  if (self->pool) {
    g_signal_handlers_disconnect_by_func (self->pool,
        G_CALLBACK (ges_pipeline_pool_manager_prepare_pipeline_removed_cb),
        self);
    g_signal_handlers_disconnect_by_func (self->pool,
        G_CALLBACK (new_pipeline_cb), self);
    g_signal_handlers_disconnect_by_func (self->pool,
        G_CALLBACK (ges_pipeline_pool_manager_deinit), self);
  }

  gst_clear_object (&self->pool);
  g_rec_mutex_unlock (&self->lock);
}

static void
ges_pipeline_pool_manager_deinit (GObject * _pool,
    GESPipelinePoolManager * self)
{
  ges_pipeline_pool_manager_clear (self);
}

void
ges_pipeline_pool_manager_set_max_preloaded_sources (GESPipelinePoolManager *
    self, guint max)
{
  g_rec_mutex_lock (&self->lock);
  self->max_preloaded_sources = max;
  g_rec_mutex_unlock (&self->lock);
}

guint
ges_pipeline_pool_manager_get_max_preloaded_sources (GESPipelinePoolManager *
    self)
{
  return self->max_preloaded_sources;
}

GESPipelinePoolManager *
ges_pipeline_pool_manager_new (GESTimeline * timeline)
{
  static gsize init = 0;
  static gboolean has_uridecodepoolsrc = FALSE;
  GESPipelinePoolManager *self;

  if (g_once_init_enter ((gsize *) & init)) {
    GstElement *tmp = gst_element_factory_make ("uridecodepoolsrc", NULL);

    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "gespipelinepoolmanager", 0,
        "gespipelinepoolmanager");

    has_uridecodepoolsrc = tmp != NULL;
    gst_clear_object (&tmp);

    g_once_init_leave ((gsize *) & init, 1);
  }

  self = g_atomic_rc_box_new0 (GESPipelinePoolManager);
  g_rec_mutex_init (&self->lock);
  self->timeline = timeline;

  if (!has_uridecodepoolsrc)
    return self;

  GstElement *uridecodepoolsrc =
      gst_element_factory_make ("uridecodepoolsrc", NULL);

  self->pooled_sources = g_array_new (100, TRUE, sizeof (PooledSource));
  g_array_set_clear_func (self->pooled_sources,
      (GDestroyNotify) pooled_source_clear);
  self->prepared_sources = g_array_new (100, TRUE, sizeof (PooledSource));
  g_array_set_clear_func (self->prepared_sources,
      (GDestroyNotify) pooled_source_clear);
  self->pending_nested_timelines =
      g_array_new (FALSE, TRUE, sizeof (NestedTimelineInfo));
  g_array_set_clear_func (self->pending_nested_timelines,
      (GDestroyNotify) nested_timeline_info_clear);
  self->registered_nested_timelines = g_ptr_array_new ();
  self->pool =
      gst_child_proxy_get_child_by_name (GST_CHILD_PROXY (uridecodepoolsrc),
      "pool");
  g_object_set (self->pool, "cleanup-timeout", 0, NULL);
  g_signal_connect (self->pool, "deinit",
      G_CALLBACK (ges_pipeline_pool_manager_deinit), self);
  g_signal_connect (self->pool, "prepared-pipeline-removed",
      G_CALLBACK (ges_pipeline_pool_manager_prepare_pipeline_removed_cb), self);
  g_signal_connect (self->pool, "new-pipeline", G_CALLBACK (new_pipeline_cb),
      self);
  gst_object_unref (uridecodepoolsrc);

  return self;
}

GESPipelinePoolManager *
ges_pipeline_pool_manager_ref (GESPipelinePoolManager * self)
{
  return g_atomic_rc_box_acquire (self);
}

static void
ges_pipeline_pool_manager_free (GESPipelinePoolManager * self)
{
  ges_pipeline_pool_manager_clear (self);
  g_rec_mutex_clear (&self->lock);
}

void
ges_pipeline_pool_manager_unref (GESPipelinePoolManager * self)
{
  g_atomic_rc_box_release_full (self,
      (GDestroyNotify) ges_pipeline_pool_manager_free);
}
