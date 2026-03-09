/* GStreamer Editing Services - Web Application
 * Copyright (C) 2026 GStreamer developers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <gst/gst.h>
#include <ges/ges.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/proxying.h>
#include <pthread.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

static GESTimeline *timeline = NULL;
static GESPipeline *ges_pipeline = NULL;
static GstElement *pipeline = NULL;
static GMainLoop *main_loop = NULL;
static GstState desired_state = GST_STATE_PAUSED;
static gint clip_counter = 0;

#ifdef __EMSCRIPTEN__
static pthread_t ges_thread;
static em_proxying_queue *proxy_queue = NULL;

/* Proxy a function call to the GES thread synchronously */
#define PROXY_CALL(func, arg) \
  emscripten_proxy_sync (proxy_queue, ges_thread, (void (*)(void*))func, arg)
#endif

/*
 * Cached timeline state - updated from the GES thread, read from JS (main thread).
 * This avoids crossing thread boundaries for read-only queries.
 */
#define MAX_LAYERS 16
#define MAX_CLIPS_PER_LAYER 64

static struct {
  int layer_count;
  int clip_count[MAX_LAYERS];
  double clip_start[MAX_LAYERS][MAX_CLIPS_PER_LAYER];
  double clip_duration[MAX_LAYERS][MAX_CLIPS_PER_LAYER];
  double timeline_duration;
  double position;
  gboolean playing;
} cached_state = { 0 };

static void
update_cached_state (void)
{
  GList *layers, *clips, *l;
  int li;

  if (!timeline || !pipeline)
    return;

  layers = ges_timeline_get_layers (timeline);
  cached_state.layer_count = MIN (g_list_length (layers), MAX_LAYERS);

  for (l = layers, li = 0; l && li < MAX_LAYERS; l = l->next, li++) {
    int ci;
    GList *c;

    clips = ges_layer_get_clips (l->data);
    cached_state.clip_count[li] = MIN (g_list_length (clips),
        MAX_CLIPS_PER_LAYER);

    for (c = clips, ci = 0; c && ci < MAX_CLIPS_PER_LAYER; c = c->next, ci++) {
      cached_state.clip_start[li][ci] =
          (double) ges_timeline_element_get_start (GES_TIMELINE_ELEMENT
          (c->data)) / GST_SECOND;
      cached_state.clip_duration[li][ci] =
          (double) ges_timeline_element_get_duration (GES_TIMELINE_ELEMENT
          (c->data)) / GST_SECOND;
    }
    g_list_free_full (clips, gst_object_unref);
  }
  g_list_free_full (layers, gst_object_unref);

  cached_state.timeline_duration =
      (double) ges_timeline_get_duration (timeline) / GST_SECOND;

  {
    gint64 pos = 0;
    if (gst_element_query_position (pipeline,
            GST_FORMAT_TIME, &pos))
      cached_state.position = (double) pos / GST_SECOND;
  }

  cached_state.playing = (desired_state == GST_STATE_PLAYING);
}

static gboolean
update_cached_state_cb (gpointer data)
{
  update_cached_state ();
  return G_SOURCE_CONTINUE;
}

static void
bus_message_cb (GstBus * bus, GstMessage * message, gpointer user_data)
{
  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:{
      GError *err = NULL;
      gchar *dbg_info = NULL;

      gst_message_parse_error (message, &err, &dbg_info);
      g_printerr ("ERROR from element %s: %s\n",
          GST_OBJECT_NAME (message->src), err->message);
      g_printerr ("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
      g_clear_error (&err);
      g_free (dbg_info);
      break;
    }
    case GST_MESSAGE_EOS:
      gst_print ("End of stream, looping...\n");
      gst_element_seek_simple (pipeline, GST_FORMAT_TIME,
          GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 0);
      break;
    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC (message) == GST_OBJECT_CAST (pipeline)) {
        GstState old_state, new_state;
        gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
        gst_print ("Pipeline state: %s -> %s\n",
            gst_state_get_name (old_state),
            gst_state_get_name (new_state));
      }
      break;
    default:
      break;
  }
}

static gboolean
setup_simple_test_pipeline (void)
{
  GstBus *bus;
  GstElement *pipe, *src, *sink;

  gst_print ("Creating simple test pipeline: videotestsrc ! glimagesink\n");

  pipe = gst_pipeline_new ("test-pipe");
  src = gst_element_factory_make ("videotestsrc", NULL);
  sink = gst_element_factory_make ("glimagesink", NULL);

  if (!pipe || !src || !sink) {
    g_printerr ("Failed to create pipeline elements (pipe=%p src=%p sink=%p)\n",
        (void*)pipe, (void*)src, (void*)sink);
    return FALSE;
  }
  gst_bin_add_many (GST_BIN (pipe), src, sink, NULL);
  if (!gst_element_link (src, sink)) {
    g_printerr ("Failed to link elements\n");
    return FALSE;
  }

  pipeline = pipe;

  bus = gst_pipeline_get_bus (GST_PIPELINE (pipe));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message", G_CALLBACK (bus_message_cb), NULL);
  gst_object_unref (bus);

  g_timeout_add (50, update_cached_state_cb, NULL);

  return TRUE;
}

static gboolean
setup_ges_pipeline (void)
{
  GstBus *bus;
  GESTrack *video_track;

  gst_print ("Creating GES pipeline\n");

  timeline = ges_timeline_new ();

  /* Add a video track */
  video_track = GES_TRACK (ges_video_track_new ());
  if (!ges_timeline_add_track (timeline, video_track)) {
    g_printerr ("Failed to add video track\n");
    return FALSE;
  }

  /* Add a default layer */
  ges_timeline_append_layer (timeline);

  /* Create the GES pipeline and set the timeline */
  ges_pipeline = ges_pipeline_new ();
  if (!ges_pipeline_set_timeline (ges_pipeline, timeline)) {
    g_printerr ("Failed to set timeline on pipeline\n");
    return FALSE;
  }

  /* Set glimagesink as the video sink (autovideosink is not available in WASM) */
  {
    GstElement *video_sink = gst_element_factory_make ("glimagesink", NULL);
    if (!video_sink) {
      g_printerr ("Failed to create glimagesink\n");
      return FALSE;
    }
    ges_pipeline_preview_set_video_sink (ges_pipeline, video_sink);
    gst_object_unref (video_sink);
  }

  /* Set preview mode */
  ges_pipeline_set_mode (ges_pipeline, GES_PIPELINE_MODE_PREVIEW_VIDEO);

  pipeline = GST_ELEMENT (ges_pipeline);

  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message", G_CALLBACK (bus_message_cb), NULL);
  gst_object_unref (bus);

  g_timeout_add (50, update_cached_state_cb, NULL);

  /* Add a default SMPTE bars clip so there's something to render */
  {
    GList *layers = ges_timeline_get_layers (timeline);
    GESLayer *layer = g_list_nth_data (layers, 0);
    GESClip *clip = GES_CLIP (ges_test_clip_new ());
    g_object_set (clip,
        "start", (guint64) 0,
        "duration", (guint64) (5 * GST_SECOND),
        "vpattern", 0, /* SMPTE bars */
        NULL);
    ges_layer_add_clip (layer, clip);
    ges_timeline_commit (timeline);
    g_list_free_full (layers, gst_object_unref);
    update_cached_state ();
  }

  gst_print ("GES pipeline created with default clip\n");
  return TRUE;
}

static gboolean
setup_pipeline (void)
{
  return setup_ges_pipeline ();
}

/* --- Read-only queries: JS reads directly from cached state --- */

EMSCRIPTEN_KEEPALIVE double
ges_web_app_get_position (void)
{
  return cached_state.position;
}

EMSCRIPTEN_KEEPALIVE double
ges_web_app_get_duration (void)
{
  return cached_state.timeline_duration;
}

EMSCRIPTEN_KEEPALIVE gboolean
ges_web_app_is_playing (void)
{
  return cached_state.playing;
}

EMSCRIPTEN_KEEPALIVE int
ges_web_app_get_layer_count (void)
{
  return cached_state.layer_count;
}

EMSCRIPTEN_KEEPALIVE int
ges_web_app_get_clip_count (int layer_index)
{
  if (layer_index < 0 || layer_index >= cached_state.layer_count)
    return 0;
  return cached_state.clip_count[layer_index];
}

EMSCRIPTEN_KEEPALIVE double
ges_web_app_get_clip_start (int layer_index, int clip_index)
{
  if (layer_index < 0 || layer_index >= cached_state.layer_count)
    return 0.0;
  if (clip_index < 0 || clip_index >= cached_state.clip_count[layer_index])
    return 0.0;
  return cached_state.clip_start[layer_index][clip_index];
}

EMSCRIPTEN_KEEPALIVE double
ges_web_app_get_clip_duration (int layer_index, int clip_index)
{
  if (layer_index < 0 || layer_index >= cached_state.layer_count)
    return 0.0;
  if (clip_index < 0 || clip_index >= cached_state.clip_count[layer_index])
    return 0.0;
  return cached_state.clip_duration[layer_index][clip_index];
}

/* --- Mutations: proxied to the GES thread --- */

typedef struct {
  int result;
  int layer_index;
  int clip_index;
  int pattern;
  double start;
  double duration;
  double position;
  const char *effect_desc;
} ProxyArgs;

static void
do_play (void *data)
{
  (void) data;
  desired_state = GST_STATE_PLAYING;
  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  gst_print ("Playing\n");
}

static void
do_pause (void *data)
{
  (void) data;
  desired_state = GST_STATE_PAUSED;
  gst_element_set_state (pipeline, GST_STATE_PAUSED);
  gst_print ("Paused\n");
}

static void
do_seek (void *data)
{
  ProxyArgs *args = data;
  gint64 pos = (gint64) (args->position * GST_SECOND);
  gst_element_seek_simple (pipeline, GST_FORMAT_TIME,
      GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, pos);
}

static void
do_add_test_clip (void *data)
{
  ProxyArgs *args = data;
  GESLayer *layer;
  GESClip *clip;
  GList *layers;

  layers = ges_timeline_get_layers (timeline);

  /* Create layers up to the requested index */
  while ((int) g_list_length (layers) <= args->layer_index) {
    g_list_free_full (layers, gst_object_unref);
    ges_timeline_append_layer (timeline);
    layers = ges_timeline_get_layers (timeline);
  }

  layer = g_list_nth_data (layers, args->layer_index);
  if (!layer) {
    g_list_free_full (layers, gst_object_unref);
    args->result = -1;
    return;
  }

  clip = GES_CLIP (ges_test_clip_new ());
  g_object_set (clip,
      "start", (guint64) (args->start * GST_SECOND),
      "duration", (guint64) (args->duration * GST_SECOND),
      "vpattern", args->pattern,
      NULL);

  if (!ges_layer_add_clip (layer, clip)) {
    g_list_free_full (layers, gst_object_unref);
    args->result = -1;
    return;
  }

  ges_timeline_commit (timeline);
  g_list_free_full (layers, gst_object_unref);
  update_cached_state ();

  args->result = clip_counter++;
}

static void
do_remove_clip (void *data)
{
  ProxyArgs *args = data;
  GESLayer *layer;
  GESClip *clip;
  GList *layers, *clips;

  layers = ges_timeline_get_layers (timeline);
  layer = g_list_nth_data (layers, args->layer_index);
  if (!layer) {
    g_list_free_full (layers, gst_object_unref);
    args->result = 0;
    return;
  }

  clips = ges_layer_get_clips (layer);
  clip = g_list_nth_data (clips, args->clip_index);
  if (clip) {
    args->result = ges_layer_remove_clip (layer, clip);
    ges_timeline_commit (timeline);
    update_cached_state ();
  } else {
    args->result = 0;
  }

  g_list_free_full (clips, gst_object_unref);
  g_list_free_full (layers, gst_object_unref);
}

static void
do_add_effect (void *data)
{
  ProxyArgs *args = data;
  GESLayer *layer;
  GESClip *clip;
  GESEffect *effect;
  GList *layers, *clips;

  layers = ges_timeline_get_layers (timeline);
  layer = g_list_nth_data (layers, args->layer_index);
  if (!layer) {
    g_list_free_full (layers, gst_object_unref);
    args->result = 0;
    return;
  }

  clips = ges_layer_get_clips (layer);
  clip = g_list_nth_data (clips, args->clip_index);
  if (!clip) {
    g_list_free_full (clips, gst_object_unref);
    g_list_free_full (layers, gst_object_unref);
    args->result = 0;
    return;
  }

  effect = ges_effect_new (args->effect_desc);
  if (effect) {
    args->result =
        ges_clip_add_top_effect (clip, GES_BASE_EFFECT (effect), -1, NULL);
    if (args->result)
      ges_timeline_commit (timeline);
    update_cached_state ();
  } else {
    args->result = 0;
  }

  g_list_free_full (clips, gst_object_unref);
  g_list_free_full (layers, gst_object_unref);
}

static void
do_add_layer (void *data)
{
  ProxyArgs *args = data;
  GESLayer *layer = ges_timeline_append_layer (timeline);
  ges_timeline_commit (timeline);
  update_cached_state ();
  args->result = ges_layer_get_priority (layer);
}

static void
do_split_clip (void *data)
{
  ProxyArgs *args = data;
  GESLayer *layer;
  GESClip *clip, *new_clip;
  GList *layers, *clips;

  layers = ges_timeline_get_layers (timeline);
  layer = g_list_nth_data (layers, args->layer_index);
  if (!layer) {
    g_list_free_full (layers, gst_object_unref);
    args->result = -1;
    return;
  }

  clips = ges_layer_get_clips (layer);
  clip = g_list_nth_data (clips, args->clip_index);
  if (!clip) {
    g_list_free_full (clips, gst_object_unref);
    g_list_free_full (layers, gst_object_unref);
    args->result = -1;
    return;
  }

  new_clip =
      ges_clip_split (clip, (guint64) (args->position * GST_SECOND));
  ges_timeline_commit (timeline);
  update_cached_state ();

  g_list_free_full (clips, gst_object_unref);
  g_list_free_full (layers, gst_object_unref);

  args->result = new_clip ? clip_counter++ : -1;
}

static void
do_set_clip_start (void *data)
{
  ProxyArgs *args = data;
  GESLayer *layer;
  GESClip *clip;
  GList *layers, *clips;

  layers = ges_timeline_get_layers (timeline);
  layer = g_list_nth_data (layers, args->layer_index);
  if (!layer) {
    g_list_free_full (layers, gst_object_unref);
    return;
  }

  clips = ges_layer_get_clips (layer);
  clip = g_list_nth_data (clips, args->clip_index);
  if (clip) {
    ges_timeline_element_set_start (GES_TIMELINE_ELEMENT (clip),
        (guint64) (args->start * GST_SECOND));
    ges_timeline_commit (timeline);
    update_cached_state ();
  }

  g_list_free_full (clips, gst_object_unref);
  g_list_free_full (layers, gst_object_unref);
}

static void
do_set_clip_duration (void *data)
{
  ProxyArgs *args = data;
  GESLayer *layer;
  GESClip *clip;
  GList *layers, *clips;

  layers = ges_timeline_get_layers (timeline);
  layer = g_list_nth_data (layers, args->layer_index);
  if (!layer) {
    g_list_free_full (layers, gst_object_unref);
    return;
  }

  clips = ges_layer_get_clips (layer);
  clip = g_list_nth_data (clips, args->clip_index);
  if (clip) {
    ges_timeline_element_set_duration (GES_TIMELINE_ELEMENT (clip),
        (guint64) (args->duration * GST_SECOND));
    ges_timeline_commit (timeline);
    update_cached_state ();
  }

  g_list_free_full (clips, gst_object_unref);
  g_list_free_full (layers, gst_object_unref);
}

/* --- JS-callable wrappers that proxy to the GES thread --- */

EMSCRIPTEN_KEEPALIVE void
ges_web_app_play (void)
{
  if (!pipeline) return;
#ifdef __EMSCRIPTEN__
  PROXY_CALL (do_play, NULL);
#else
  do_play (NULL);
#endif
}

EMSCRIPTEN_KEEPALIVE void
ges_web_app_pause (void)
{
  if (!pipeline) return;
#ifdef __EMSCRIPTEN__
  PROXY_CALL (do_pause, NULL);
#else
  do_pause (NULL);
#endif
}

EMSCRIPTEN_KEEPALIVE void
ges_web_app_toggle_playback (void)
{
  if (desired_state == GST_STATE_PLAYING)
    ges_web_app_pause ();
  else
    ges_web_app_play ();
}

EMSCRIPTEN_KEEPALIVE void
ges_web_app_seek (double position_seconds)
{
  if (!pipeline) return;
  ProxyArgs args = { .position = position_seconds };
#ifdef __EMSCRIPTEN__
  PROXY_CALL (do_seek, &args);
#else
  do_seek (&args);
#endif
}

EMSCRIPTEN_KEEPALIVE int
ges_web_app_add_test_clip (int layer_index, int pattern,
    double start_seconds, double duration_seconds)
{
  if (!timeline) return -1;
  ProxyArgs args = {
    .layer_index = layer_index,
    .pattern = pattern,
    .start = start_seconds,
    .duration = duration_seconds,
    .result = -1
  };
#ifdef __EMSCRIPTEN__
  PROXY_CALL (do_add_test_clip, &args);
#else
  do_add_test_clip (&args);
#endif
  return args.result;
}

EMSCRIPTEN_KEEPALIVE gboolean
ges_web_app_remove_clip (int layer_index, int clip_index)
{
  if (!timeline) return FALSE;
  ProxyArgs args = {
    .layer_index = layer_index,
    .clip_index = clip_index,
    .result = 0
  };
#ifdef __EMSCRIPTEN__
  PROXY_CALL (do_remove_clip, &args);
#else
  do_remove_clip (&args);
#endif
  return args.result;
}

EMSCRIPTEN_KEEPALIVE gboolean
ges_web_app_add_effect (int layer_index, int clip_index,
    const char *effect_desc)
{
  if (!timeline || !effect_desc) return FALSE;
  ProxyArgs args = {
    .layer_index = layer_index,
    .clip_index = clip_index,
    .effect_desc = effect_desc,
    .result = 0
  };
#ifdef __EMSCRIPTEN__
  PROXY_CALL (do_add_effect, &args);
#else
  do_add_effect (&args);
#endif
  return args.result;
}

EMSCRIPTEN_KEEPALIVE void
ges_web_app_set_clip_duration (int layer_index, int clip_index,
    double duration_seconds)
{
  if (!timeline) return;
  ProxyArgs args = {
    .layer_index = layer_index,
    .clip_index = clip_index,
    .duration = duration_seconds
  };
#ifdef __EMSCRIPTEN__
  PROXY_CALL (do_set_clip_duration, &args);
#else
  do_set_clip_duration (&args);
#endif
}

EMSCRIPTEN_KEEPALIVE void
ges_web_app_set_clip_start (int layer_index, int clip_index,
    double start_seconds)
{
  if (!timeline) return;
  ProxyArgs args = {
    .layer_index = layer_index,
    .clip_index = clip_index,
    .start = start_seconds
  };
#ifdef __EMSCRIPTEN__
  PROXY_CALL (do_set_clip_start, &args);
#else
  do_set_clip_start (&args);
#endif
}

EMSCRIPTEN_KEEPALIVE int
ges_web_app_add_layer (void)
{
  if (!timeline) return -1;
  ProxyArgs args = { .result = -1 };
#ifdef __EMSCRIPTEN__
  PROXY_CALL (do_add_layer, &args);
#else
  do_add_layer (&args);
#endif
  return args.result;
}

EMSCRIPTEN_KEEPALIVE int
ges_web_app_split_clip (int layer_index, int clip_index,
    double position_seconds)
{
  if (!timeline) return -1;
  ProxyArgs args = {
    .layer_index = layer_index,
    .clip_index = clip_index,
    .position = position_seconds,
    .result = -1
  };
#ifdef __EMSCRIPTEN__
  PROXY_CALL (do_split_clip, &args);
#else
  do_split_clip (&args);
#endif
  return args.result;
}

int
main (int argc, char *argv[])
{
#ifdef __EMSCRIPTEN__
  ges_thread = pthread_self ();
  proxy_queue = emscripten_proxy_get_system_queue ();
#endif

  g_setenv ("GST_DEBUG", "2,ges:3,glcontext:3,glwindow:3", TRUE);
  gst_init (&argc, &argv);

  if (!ges_init ()) {
    g_printerr ("Failed to initialize GES\n");
    return 1;
  }

  gst_print ("GStreamer Editing Services Web App initialized\n");

  if (!setup_pipeline ()) {
    g_printerr ("Failed to setup pipeline\n");
    return 1;
  }

  gst_print ("Pipeline created, starting playback...\n");

  /* Start playing (called directly since we're on the GES thread) */
  do_play (NULL);

  /* Run the GLib main loop */
  main_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (main_loop);

  /* Cleanup */
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
  g_main_loop_unref (main_loop);

  ges_deinit ();
  gst_deinit ();

  return 0;
}
