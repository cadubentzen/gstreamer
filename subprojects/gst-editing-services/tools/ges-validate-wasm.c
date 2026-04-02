/* GStreamer Editing Services
 *
 * Copyright (C) 2025 Thibault Saunier <tsaunier@igalia.com>
 *
 * ges-validate-wasm.c - GES Validate runner for WASM/browser
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <emscripten.h>
#include <emscripten/stack.h>
#include <gst/gst.h>
#include <gst/gl/gl.h>
#include <gst/gl/web/gstgldisplay_web.h>
#include <gst/validate/validate.h>
#include <gst/validate/gst-validate-scenario.h>
#include <gst/validate/gst-validate-utils.h>
#include <gst/validate/gst-validate-pipeline-monitor.h>
#include <ges/ges.h>

GST_DEBUG_CATEGORY_STATIC (ges_validate_wasm_dbg);
#define GST_CAT_DEFAULT ges_validate_wasm_dbg

static gint ret = 0;
static GMainLoop *mainloop;
static GstElement *pipeline;
static GESTimeline *s_timeline;

#define MAX_LAYERS 16
#define MAX_CLIPS_PER_LAYER 64

static struct {
  int layer_count;
  int clip_count[MAX_LAYERS];
  double clip_start[MAX_LAYERS][MAX_CLIPS_PER_LAYER];
  double clip_duration[MAX_LAYERS][MAX_CLIPS_PER_LAYER];
  const gchar *clip_name[MAX_LAYERS][MAX_CLIPS_PER_LAYER];
  double timeline_duration;
  double position;
} cached_state = { 0 };

static void
update_cached_state (void)
{
  GList *layers, *l;
  int li;

  if (!s_timeline || !pipeline)
    return;

  layers = ges_timeline_get_layers (s_timeline);
  cached_state.layer_count = MIN ((int) g_list_length (layers), MAX_LAYERS);

  for (l = layers, li = 0; l && li < MAX_LAYERS; l = l->next, li++) {
    GList *clips, *c;
    int ci;

    clips = ges_layer_get_clips (l->data);
    cached_state.clip_count[li] = MIN ((int) g_list_length (clips),
        MAX_CLIPS_PER_LAYER);

    for (c = clips, ci = 0; c && ci < MAX_CLIPS_PER_LAYER; c = c->next, ci++) {
      cached_state.clip_start[li][ci] =
          (double) ges_timeline_element_get_start (GES_TIMELINE_ELEMENT
          (c->data)) / GST_SECOND;
      cached_state.clip_duration[li][ci] =
          (double) ges_timeline_element_get_duration (GES_TIMELINE_ELEMENT
          (c->data)) / GST_SECOND;
      cached_state.clip_name[li][ci] =
          ges_timeline_element_get_name (GES_TIMELINE_ELEMENT (c->data));
    }
    g_list_free_full (clips, gst_object_unref);
  }
  g_list_free_full (layers, gst_object_unref);

  cached_state.timeline_duration =
      (double) ges_timeline_get_duration (s_timeline) / GST_SECOND;

  {
    gint64 pos = 0;
    if (gst_element_query_position (pipeline, GST_FORMAT_TIME, &pos))
      cached_state.position = (double) pos / GST_SECOND;
  }
}

static gboolean
update_cached_state_cb (gpointer data)
{
  update_cached_state ();
  return G_SOURCE_CONTINUE;
}

double
ges_validate_wasm_get_position (void)
{
  return cached_state.position;
}

double
ges_validate_wasm_get_duration (void)
{
  return cached_state.timeline_duration;
}

int
ges_validate_wasm_get_layer_count (void)
{
  return cached_state.layer_count;
}

int
ges_validate_wasm_get_clip_count (int layer)
{
  if (layer < 0 || layer >= cached_state.layer_count)
    return 0;
  return cached_state.clip_count[layer];
}

double
ges_validate_wasm_get_clip_start (int layer, int clip)
{
  if (layer < 0 || layer >= cached_state.layer_count)
    return 0;
  if (clip < 0 || clip >= cached_state.clip_count[layer])
    return 0;
  return cached_state.clip_start[layer][clip];
}

double
ges_validate_wasm_get_clip_duration (int layer, int clip)
{
  if (layer < 0 || layer >= cached_state.layer_count)
    return 0;
  if (clip < 0 || clip >= cached_state.clip_count[layer])
    return 0;
  return cached_state.clip_duration[layer][clip];
}

const gchar *
ges_validate_wasm_get_clip_name (int layer, int clip)
{
  if (layer < 0 || layer >= cached_state.layer_count)
    return "";
  if (clip < 0 || clip >= cached_state.clip_count[layer])
    return "";
  return cached_state.clip_name[layer][clip] ? cached_state.clip_name[layer][clip] : "";
}

typedef struct
{
  GMainLoop *mainloop;
  GstValidateMonitor *monitor;
} BusCallbackData;

static GstGLDisplay *shared_gl_display = NULL;

static GstBusSyncReply
sync_bus_handler (GstBus * bus, GstMessage * msg, gpointer user_data)
{
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_NEED_CONTEXT && shared_gl_display) {
    const gchar *ctx_type;
    gst_message_parse_context_type (msg, &ctx_type);
    if (g_strcmp0 (ctx_type, GST_GL_DISPLAY_CONTEXT_TYPE) == 0) {
      GstContext *ctx = gst_context_new (GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
      gst_context_set_gl_display (ctx, shared_gl_display);
      gst_element_set_context (GST_ELEMENT (GST_MESSAGE_SRC (msg)), ctx);
      gst_context_unref (ctx);
      return GST_BUS_DROP;
    }
  }
  return GST_BUS_PASS;
}

static void
bus_callback (GstBus * bus, GstMessage * message, gpointer data)
{
  BusCallbackData *bus_callback_data = data;
  GMainLoop *loop = bus_callback_data->mainloop;
  GstValidateMonitor *monitor = bus_callback_data->monitor;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_NEED_CONTEXT:
    {
      const gchar *context_type;
      gst_message_parse_context_type (message, &context_type);

      if (shared_gl_display &&
          g_strcmp0 (context_type, GST_GL_DISPLAY_CONTEXT_TYPE) == 0) {
        GstContext *ctx = gst_context_new (GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
        gst_context_set_gl_display (ctx, shared_gl_display);
        gst_element_set_context (GST_ELEMENT (GST_MESSAGE_SRC (message)), ctx);
        gst_context_unref (ctx);
      }
      break;
    }
    case GST_MESSAGE_ERROR:
    {
      GError *err = NULL;
      gchar *dbg = NULL;

      gst_message_parse_error (message, &err, &dbg);
      GST_ERROR ("ERROR: %s (debug: %s)", err->message,
          dbg ? dbg : "none");
      g_clear_error (&err);
      g_free (dbg);
      ret = -1;
      g_main_loop_quit (loop);
      break;
    }
    case GST_MESSAGE_EOS:
      GST_INFO ("Got EOS");
      g_main_loop_quit (loop);
      break;
    case GST_MESSAGE_LATENCY:
      gst_bin_recalculate_latency (GST_BIN (pipeline));
      break;
    case GST_MESSAGE_STATE_CHANGED:
      if (GST_MESSAGE_SRC (message) == GST_OBJECT (pipeline)) {
        GstState oldstate, newstate, pending;
        gst_message_parse_state_changed (message, &oldstate, &newstate,
            &pending);
        GST_DEBUG ("State changed (old: %s, new: %s, pending: %s)",
            gst_element_state_get_name (oldstate),
            gst_element_state_get_name (newstate), gst_element_state_get_name (pending));
      }
      break;
    case GST_MESSAGE_BUFFERING:
    {
      gint percent;
      GstBufferingMode mode;
      GstState target_state = GST_STATE_PLAYING;
      gboolean monitor_handles_state;

      g_object_get (monitor, "handles-states", &monitor_handles_state, NULL);
      if (monitor_handles_state && GST_IS_VALIDATE_BIN_MONITOR (monitor)) {
        target_state =
            gst_validate_scenario_get_target_state (GST_VALIDATE_BIN_MONITOR
            (monitor)->scenario);
      }

      gst_message_parse_buffering (message, &percent);
      gst_message_parse_buffering_stats (message, &mode, NULL, NULL, NULL);

      if (mode == GST_BUFFERING_LIVE)
        break;

      if (percent == 100) {
        if (target_state == GST_STATE_PLAYING)
          gst_element_set_state (pipeline, GST_STATE_PLAYING);
      } else {
        gst_element_set_state (pipeline, GST_STATE_PAUSED);
      }
      break;
    }
    case GST_MESSAGE_REQUEST_STATE:
    {
      GstState state;

      gst_message_parse_request_state (message, &state);
      if (GST_IS_VALIDATE_SCENARIO (GST_MESSAGE_SRC (message))
          && state == GST_STATE_NULL) {
        GST_INFO ("Scenario requested NULL state, quitting mainloop");
        g_main_loop_quit (mainloop);
      }
      break;
    }
    default:
      break;
  }
}

static GESTimeline *
create_timeline (gchar ** tokens, gint n_tokens)
{
  GESTimeline *timeline = NULL;
  GESProject *project;
  gint i;

  /* Create a video-only timeline with a default layer.
   * Clips are added by validate scenario actions (add-clip). */
  timeline = ges_timeline_new ();
  ges_timeline_add_track (timeline, GES_TRACK (ges_video_track_new ()));
  ges_timeline_append_layer (timeline);

  return timeline;
}

static void
setup_gl_display (void)
{
  GstGLDisplayWeb *gl_display;
  GstContext *display_context;

  gl_display = gst_gl_display_web_new ((gpointer) "#canvas");
  shared_gl_display = GST_GL_DISPLAY (gl_display);
  display_context = gst_context_new (GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
  gst_context_set_gl_display (display_context, shared_gl_display);
  gst_element_set_context (pipeline, display_context);
  gst_context_unref (display_context);
}

static void
configure_sinks (GESPipeline * ges_pipeline, gchar ** tokens, gint n_tokens)
{
  GstElement *videosink = NULL, *audiosink = NULL;
  gint i;

  for (i = 0; i < n_tokens; i++) {
    if (g_strcmp0 (tokens[i], "--videosink") == 0 && i + 1 < n_tokens) {
      videosink = gst_parse_launch (tokens[i + 1], NULL);
    } else if (g_strcmp0 (tokens[i], "--audiosink") == 0 && i + 1 < n_tokens) {
      audiosink = gst_parse_launch (tokens[i + 1], NULL);
    }
  }

  if (!videosink)
    videosink = gst_element_factory_make ("glimagesink", NULL);
  if (!audiosink)
    audiosink = gst_element_factory_make ("fakesink", NULL);

  ges_pipeline_preview_set_video_sink (ges_pipeline, videosink);
  ges_pipeline_preview_set_audio_sink (ges_pipeline, audiosink);
}

int
main (int argc, char **argv)
{
  GError *err = NULL;
  gchar **args;
  GstValidateRunner *runner;
  GstValidateMonitor *monitor;
  GstBus *bus;
  BusCallbackData bus_callback_data = { 0, };
  gboolean monitor_handles_state;
  GstStateChangeReturn sret;
  int rep_err;
  GstStructure *meta;
  GESTimeline *timeline;
  GESPipeline *ges_pipeline;

  g_set_prgname ("ges-validate-wasm-" GST_API_VERSION);

  {
    size_t stack_base = (size_t) emscripten_stack_get_base ();
    size_t stack_end = (size_t) emscripten_stack_get_end ();
    size_t stack_size = stack_base - stack_end;
    printf ("WASM stack: base=%zu end=%zu size=%zu (%.1fMB)\n",
        stack_base, stack_end, stack_size,
        (double) stack_size / 1024.0 / 1024.0);
    fflush (stdout);
  }

  gst_init (NULL, NULL);
  ges_init ();

  GST_DEBUG_CATEGORY_INIT (ges_validate_wasm_dbg, "ges-validate-wasm", 0,
      "GES Validate WASM runner");

  gst_validate_init_debug ();

  /* Load test file BEFORE gst_validate_init (required by API) */
  meta = gst_validate_setup_test_file ("/test.validatetest", FALSE);
  if (!meta)
    gst_validate_abort ("Failed to load test file");

  args = gst_validate_utils_get_strv (meta, "args");
  if (!args)
    gst_validate_abort ("No 'args' in test file meta");

  /* Process GES-specific configuration (converter-type, compositor-factory) */
  {
    const gchar *ges_config = gst_structure_get_string (meta, "ges");
    if (ges_config) {
      gchar *struct_str = g_strdup_printf ("ges,%s", ges_config);
      GstStructure *ges_struct = gst_structure_from_string (struct_str, NULL);
      if (ges_struct) {
        const gchar *converter = gst_structure_get_string (ges_struct, "converter-type");
        if (converter)
          g_setenv ("GES_CONVERTER_TYPE", converter, TRUE);

        const gchar *compositor = gst_structure_get_string (ges_struct, "compositor-factory");
        if (compositor) {
          GstElementFactory *factory = gst_element_factory_find (compositor);
          if (factory) {
            gst_plugin_feature_set_rank (GST_PLUGIN_FEATURE (factory),
                GST_RANK_PRIMARY + 1000);
            gst_object_unref (factory);
          }
        }
        gst_structure_free (ges_struct);
      }
      g_free (struct_str);
    }
  }

  /* Register GES action types BEFORE gst_validate_init processes the
   * test file scenario. ges_validate_register_action_types calls
   * gst_validate_init internally which then parses the scenario. */
  ges_validate_register_action_types ();

  runner = gst_validate_runner_new ();
  if (!runner) {
    GST_ERROR ("Failed to setup Validate Runner");
    return 1;
  }

  /* Tokenize args[0] — it's a single command string */
  gchar **tokens = NULL;
  gint n_tokens = 0;
  if (!g_shell_parse_argv (args[0], &n_tokens, &tokens, NULL) || !tokens) {
    GST_ERROR ("Failed to parse args: %s", args[0]);
    return 1;
  }

  /* Create GES timeline */
  timeline = create_timeline (tokens, n_tokens);
  s_timeline = timeline;
  if (!timeline) {
    GST_ERROR ("Failed to create timeline");
    g_strfreev (tokens);
    return 1;
  }

  /* Create GES pipeline — set up GL display BEFORE adding timeline
   * so all internal elements get the shared WebGL context. */
  ges_pipeline = ges_pipeline_new ();
  pipeline = GST_ELEMENT (ges_pipeline);
  setup_gl_display ();

  configure_sinks (ges_pipeline, tokens, n_tokens);

  if (!ges_pipeline_set_timeline (ges_pipeline, timeline)) {
    GST_ERROR ("Failed to set timeline on pipeline");
    gst_object_unref (timeline);
    gst_object_unref (ges_pipeline);
    return 1;
  }

  gst_pipeline_set_auto_flush_bus (GST_PIPELINE (pipeline), FALSE);

  /* Sync bus handler for immediate GL context delivery */
  {
    GstBus *sync_bus = gst_element_get_bus (pipeline);
    gst_bus_set_sync_handler (sync_bus, sync_bus_handler, NULL, NULL);
    gst_object_unref (sync_bus);
  }

  gst_validate_spin_on_fault_signals ();

  monitor = gst_validate_monitor_factory_create (GST_OBJECT_CAST (pipeline),
      runner, NULL);
  gst_validate_reporter_set_handle_g_logs (GST_VALIDATE_REPORTER (monitor));

  {
    GstValidateVerbosityFlags verbosity;
    g_object_get (monitor, "verbosity", &verbosity, NULL);
    verbosity &= ~GST_VALIDATE_VERBOSITY_POSITION;
    g_object_set (monitor, "verbosity", verbosity, NULL);
  }

  mainloop = g_main_loop_new (NULL, FALSE);
  bus = gst_element_get_bus (pipeline);
  gst_bus_add_signal_watch (bus);
  bus_callback_data.mainloop = mainloop;
  bus_callback_data.monitor = monitor;
  g_signal_connect (bus, "message", (GCallback) bus_callback,
      &bus_callback_data);

  GST_INFO ("Starting GES pipeline");
  g_object_get (monitor, "handles-states", &monitor_handles_state, NULL);
  if (!monitor_handles_state) {
    sret = gst_element_set_state (pipeline, GST_STATE_PLAYING);
    switch (sret) {
      case GST_STATE_CHANGE_FAILURE:
        GST_ERROR ("Pipeline failed to go to PLAYING state");
        gst_element_set_state (pipeline, GST_STATE_NULL);
        ret = -1;
        goto exit;
      default:
        break;
    }
  }

  g_timeout_add (50, update_cached_state_cb, NULL);
  g_main_loop_run (mainloop);

  gst_validate_printf (NULL, "\n=======> Test %s (Return value: %i)\n\n",
      ret == 0 ? "PASSED" : "FAILED", ret);

  /* clang-format off */
  MAIN_THREAD_ASYNC_EM_ASM({
    window._gstValidateResult = $0;
  }, ret);
  /* clang-format on */

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_element_get_state (pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);

  rep_err = gst_validate_runner_exit (runner, TRUE);
  if (ret == 0)
    ret = rep_err;

  gst_validate_printf (NULL, "\n=======> Test %s (Return value: %i)\n\n",
      ret == 0 ? "PASSED" : "FAILED", ret);

  gst_bus_set_flushing (bus, TRUE);
  gst_bus_remove_signal_watch (bus);
  gst_object_unref (bus);

exit:
  g_main_loop_unref (mainloop);
  g_object_unref (pipeline);
  g_object_unref (runner);
  gst_validate_reporter_purge_reports (GST_VALIDATE_REPORTER (monitor));
  g_object_unref (monitor);
  g_strfreev (args);

  gst_validate_deinit ();
  ges_deinit ();
  gst_deinit ();

  return ret;
}
