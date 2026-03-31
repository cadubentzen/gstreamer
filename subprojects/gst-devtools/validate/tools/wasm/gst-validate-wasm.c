/* GStreamer
 *
 * Copyright (C) 2025 Thibault Saunier <tsaunier@igalia.com>
 *
 * gst-validate-wasm.c - Validate runner for WASM/WebCodecs
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
#include <gst/video/videooverlay.h>
#include <gst/gl/gl.h>
#include <gst/gl/web/gstgldisplay_web.h>
#include <gst/validate/validate.h>
#include <gst/validate/gst-validate-scenario.h>
#include <gst/validate/gst-validate-utils.h>
#include <gst/validate/gst-validate-pipeline-monitor.h>

GST_DEBUG_CATEGORY_STATIC (validate_wasm_dbg);
#define GST_CAT_DEFAULT validate_wasm_dbg

static gint ret = 0;
static GMainLoop *mainloop;
static GstElement *pipeline;

/* no extra measurement functions needed */

#ifdef HAVE_RSVALIDATE
void gst_plugin_rsvalidate_register (void);
#endif

typedef struct
{
  GMainLoop *mainloop;
  GstValidateMonitor *monitor;
} BusCallbackData;

static GstGLDisplay *shared_gl_display = NULL;

static int sync_handler_depth = 0;

static GstBusSyncReply
sync_bus_handler (GstBus * bus, GstMessage * msg, gpointer user_data)
{
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_NEED_CONTEXT && shared_gl_display) {
    const gchar *ctx_type;
    gst_message_parse_context_type (msg, &ctx_type);
    if (g_strcmp0 (ctx_type, GST_GL_DISPLAY_CONTEXT_TYPE) == 0) {
      GstContext *ctx = gst_context_new (GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
      gst_context_set_gl_display (ctx, shared_gl_display);
      sync_handler_depth++;
      EM_ASM({ console.error("sync_bus_handler depth=" + $0 + " element=" + UTF8ToString($1)); },
          sync_handler_depth, GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)));
      gst_element_set_context (GST_ELEMENT (GST_MESSAGE_SRC (msg)), ctx);
      sync_handler_depth--;
      gst_context_unref (ctx);
      GST_DEBUG ("Provided GL display to %" GST_PTR_FORMAT,
          GST_MESSAGE_SRC (msg));
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

  g_set_prgname ("gst-validate-" GST_API_VERSION);

  {
    size_t stack_base = (size_t) emscripten_stack_get_base ();
    size_t stack_end = (size_t) emscripten_stack_get_end ();
    size_t stack_size = stack_base - stack_end;
    printf ("WASM stack: base=%zu end=%zu size=%zu (%.1fMB)\n",
        stack_base, stack_end, stack_size,
        (double) stack_size / 1024.0 / 1024.0);
    fflush (stdout);
  }

  /* Measure V8 native stack depth on this pthread worker */
  EM_ASM({
    function measureJsDepth() {
      var d = 0;
      function f() { d++; f(); }
      try { f(); } catch(e) {}
      return d;
    }
    var jsDepth = measureJsDepth();
    console.log("V8 JS stack depth on pthread: " + jsDepth + " frames");
  });
  /* empty — measurement code removed */

  gst_init (NULL, NULL);
  GST_DEBUG_CATEGORY_INIT (validate_wasm_dbg, "validate-wasm", 0,
      "GstValidate WASM runner");

  /* g_main_loop_run() uses emscripten_set_main_loop internally via the
   * GLib ASYNCIFY patch, so no need for gst_emscripten_init(). */

  gst_validate_init_debug ();

  /* Load the test file from the preloaded virtual filesystem */
  meta = gst_validate_setup_test_file ("/test.validatetest", FALSE);
  if (!meta)
    gst_validate_abort ("Failed to load test file");

  args = gst_validate_utils_get_strv (meta, "args");
  if (!args)
    gst_validate_abort ("No 'args' in test file meta");

  gst_validate_init ();

#ifdef HAVE_RSVALIDATE
  gst_plugin_rsvalidate_register ();
#endif

  runner = gst_validate_runner_new ();
  if (!runner) {
    GST_ERROR ("Failed to setup Validate Runner");
    return 1;
  }

  /* Create the pipeline */
  GST_INFO ("Creating pipeline: %s", args[0]);
  pipeline = gst_parse_launch (args[0], &err);
  if (!pipeline) {
    GST_ERROR ("Failed to create pipeline: %s",
        err ? err->message : "unknown");
    g_clear_error (&err);
    return 1;
  }
  if (err) {
    GST_WARNING ("Erroneous pipeline: %s", err->message);
    g_clear_error (&err);
  }

  if (!GST_IS_PIPELINE (pipeline)) {
    GstElement *new_pipeline = gst_pipeline_new ("");
    gst_bin_add (GST_BIN (new_pipeline), pipeline);
    pipeline = new_pipeline;
  }

  /* Provide a GL display targeting the dedicated canvas element so that
   * glimagesink (and any GL element) creates its WebGL context on
   * #gst-gl-canvas instead of the default #canvas. */
  {
    GstGLDisplayWeb *gl_display;
    GstContext *display_context;

    gl_display = gst_gl_display_web_new ((gpointer) "#canvas");
    shared_gl_display = GST_GL_DISPLAY (gl_display);
    display_context = gst_context_new (GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
    gst_context_set_gl_display (display_context, shared_gl_display);
    gst_element_set_context (pipeline, display_context);
    gst_context_unref (display_context);
    /* Keep gl_display alive via shared_gl_display — don't unref */
  }

  gst_pipeline_set_auto_flush_bus (GST_PIPELINE (pipeline), FALSE);

  /* Set a sync bus handler so NEED_CONTEXT messages are handled
   * immediately during state changes (before the main loop runs). */
  {
    GstBus *sync_bus = gst_element_get_bus (pipeline);
    gst_bus_set_sync_handler (sync_bus, sync_bus_handler, NULL, NULL);
    gst_object_unref (sync_bus);
  }

  gst_validate_spin_on_fault_signals ();

  monitor = gst_validate_monitor_factory_create (GST_OBJECT_CAST (pipeline),
      runner, NULL);
  gst_validate_reporter_set_handle_g_logs (GST_VALIDATE_REPORTER (monitor));

  /* Disable position verbosity to avoid gst_element_query_duration()
   * during sync bus EOS handling — the combined call depth of EOS
   * processing + duration query exceeds Mac Chrome's V8 worker stack. */
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

  GST_INFO ("Starting pipeline");
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

  /* Signal result immediately after mainloop exits.
   * Use emscripten_dispatch_to_thread_async to post to the main thread
   * without blocking — MAIN_THREAD_ASYNC_EM_ASM may not fire if the
   * main thread is busy processing GL proxy calls. */
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

  /* Clean up */
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
  gst_deinit ();

  return ret;
}
