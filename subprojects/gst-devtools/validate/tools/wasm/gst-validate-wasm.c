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

#include <emscripten.h>
#include <gst/gst.h>
#include <gst/validate/validate.h>
#include <gst/validate/gst-validate-scenario.h>
#include <gst/validate/gst-validate-utils.h>
#include <gst/validate/gst-validate-pipeline-monitor.h>

GST_DEBUG_CATEGORY_STATIC (validate_wasm_dbg);
#define GST_CAT_DEFAULT validate_wasm_dbg

static gint ret = 0;
static GMainLoop *mainloop;
static GstElement *pipeline;

/* No manual element registration needed — gst-full's
 * gst_init_static_plugins() handles everything */

typedef struct
{
  GMainLoop *mainloop;
  GstValidateMonitor *monitor;
} BusCallbackData;

static void
bus_callback (GstBus * bus, GstMessage * message, gpointer data)
{
  BusCallbackData *bus_callback_data = data;
  GMainLoop *loop = bus_callback_data->mainloop;
  GstValidateMonitor *monitor = bus_callback_data->monitor;

  switch (GST_MESSAGE_TYPE (message)) {
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

  gst_pipeline_set_auto_flush_bus (GST_PIPELINE (pipeline), FALSE);
  gst_validate_spin_on_fault_signals ();

  monitor = gst_validate_monitor_factory_create (GST_OBJECT_CAST (pipeline),
      runner, NULL);
  gst_validate_reporter_set_handle_g_logs (GST_VALIDATE_REPORTER (monitor));

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
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_element_get_state (pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);

  /* Clean up */
  gst_bus_set_flushing (bus, TRUE);
  gst_bus_remove_signal_watch (bus);
  gst_object_unref (bus);

  rep_err = gst_validate_runner_exit (runner, TRUE);
  if (ret == 0)
    ret = rep_err;

exit:
  g_main_loop_unref (mainloop);
  g_object_unref (pipeline);
  g_object_unref (runner);
  gst_validate_reporter_purge_reports (GST_VALIDATE_REPORTER (monitor));
  g_object_unref (monitor);
  g_strfreev (args);

  gst_validate_printf (NULL, "\n=======> Test %s (Return value: %i)\n\n",
      ret == 0 ? "PASSED" : "FAILED", ret);

  /* Signal result to browser */
  /* clang-format off */
  MAIN_THREAD_ASYNC_EM_ASM({
    window._gstValidateResult = $0;
  }, ret);
  /* clang-format on */

  gst_validate_deinit ();
  gst_deinit ();
  return ret;
}
