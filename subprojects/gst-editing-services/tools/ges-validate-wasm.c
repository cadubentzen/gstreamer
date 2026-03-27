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
            gst_state_get_name (oldstate),
            gst_state_get_name (newstate), gst_state_get_name (pending));
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
create_timeline (gchar ** args)
{
  GESTimeline *timeline = NULL;
  GESProject *project;
  gint i;

  /* Look for a project URI in args (-l <uri> or --load <uri>) */
  for (i = 0; args[i]; i++) {
    if ((g_strcmp0 (args[i], "-l") == 0 || g_strcmp0 (args[i], "--load") == 0)
        && args[i + 1]) {
      const gchar *uri = args[i + 1];
      GST_INFO ("Loading project from %s", uri);
      project = ges_project_new (uri);
      timeline =
          GES_TIMELINE (ges_asset_extract (GES_ASSET (project), NULL));
      gst_object_unref (project);
      if (timeline)
        return timeline;
      GST_ERROR ("Failed to load project from %s", uri);
      return NULL;
    }
  }

  /* No project specified — create a simple timeline with a test clip */
  GST_INFO ("No project specified, creating test timeline");
  timeline = ges_timeline_new_audio_video ();

  {
    GESLayer *layer = ges_timeline_append_layer (timeline);
    GESClip *clip =
        GES_CLIP (ges_test_clip_new_for_nick ((gchar *) "smpte"));
    g_object_set (clip, "duration", (guint64) 5 * GST_SECOND, NULL);
    ges_layer_add_clip (layer, clip);
  }

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
configure_sinks (GESPipeline * ges_pipeline, gchar ** args)
{
  GstElement *videosink = NULL, *audiosink = NULL;
  gint i;

  for (i = 0; args[i]; i++) {
    if (g_strcmp0 (args[i], "--videosink") == 0 && args[i + 1]) {
      videosink = gst_parse_launch (args[i + 1], NULL);
    } else if (g_strcmp0 (args[i], "--audiosink") == 0 && args[i + 1]) {
      audiosink = gst_parse_launch (args[i + 1], NULL);
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

  /* Load the test file from the preloaded virtual filesystem */
  meta = gst_validate_setup_test_file ("/test.validatetest", FALSE);
  if (!meta)
    gst_validate_abort ("Failed to load test file");

  args = gst_validate_utils_get_strv (meta, "args");
  if (!args)
    gst_validate_abort ("No 'args' in test file meta");

  gst_validate_init ();

  /* Register GES-specific validate actions (add-clip, split-clip, etc.) */
  ges_validate_register_action_types ();

  runner = gst_validate_runner_new ();
  if (!runner) {
    GST_ERROR ("Failed to setup Validate Runner");
    return 1;
  }

  /* Create GES timeline */
  timeline = create_timeline (args);
  if (!timeline) {
    GST_ERROR ("Failed to create timeline");
    return 1;
  }

  /* Create GES pipeline — set up GL display BEFORE adding timeline
   * so all internal elements get the shared WebGL context. */
  ges_pipeline = ges_pipeline_new ();
  pipeline = GST_ELEMENT (ges_pipeline);
  setup_gl_display ();

  configure_sinks (ges_pipeline, args);

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
