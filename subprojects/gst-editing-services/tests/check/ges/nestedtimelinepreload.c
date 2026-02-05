/* GStreamer Editing Services
 * Copyright (C) 2026 Thibault Saunier <tsaunier@igalia.com>
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

#include "test-utils.h"

#include <ges/ges.h>
#include <gst/check/gstcheck.h>

/* Test that gessrc posts a GESNewTimeline element message when a timeline
 * is set on it. This message is used by the pipeline pool manager to
 * discover nested timelines for preloading. */
GST_START_TEST (test_gessrc_posts_new_timeline_message)
{
  GESTimeline *timeline;
  GstElement *pipeline, *gessrc, *fakesink;
  GstBus *bus;
  GstMessage *msg;
  gboolean found = FALSE;

  ges_init ();

  pipeline = gst_pipeline_new ("test-pipeline");
  gessrc = gst_element_factory_make ("gessrc", NULL);
  fail_unless (gessrc != NULL, "Failed to create gessrc element");

  fakesink = gst_element_factory_make ("fakesink", NULL);
  fail_unless (fakesink != NULL, "Failed to create fakesink element");

  gst_bin_add_many (GST_BIN (pipeline), gessrc, fakesink, NULL);

  timeline = ges_timeline_new_audio_video ();
  fail_unless (timeline != NULL, "Failed to create timeline");

  g_object_set (gessrc, "timeline", timeline, NULL);

  bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  fail_unless (bus != NULL, "Failed to get bus");

  /* Poll bus for messages until we find GESNewTimeline or timeout */
  while ((msg = gst_bus_pop (bus)) != NULL) {
    if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ELEMENT) {
      const GstStructure *s = gst_message_get_structure (msg);
      if (gst_structure_has_name (s, "GESNewTimeline")) {
        GESTimeline *msg_timeline = NULL;
        gst_structure_get (s, "timeline", GES_TYPE_TIMELINE, &msg_timeline,
            NULL);
        fail_unless (msg_timeline == timeline,
            "Timeline in message doesn't match expected timeline");
        gst_object_unref (msg_timeline);
        found = TRUE;
        gst_message_unref (msg);
        break;
      }
    }
    gst_message_unref (msg);
  }

  fail_unless (found, "GESNewTimeline message not found on bus");

  gst_object_unref (bus);
  gst_object_unref (pipeline);

  ges_deinit ();
}

GST_END_TEST;

static Suite *
ges_suite (void)
{
  Suite *s = suite_create ("ges-nestedtimelinepreload");
  TCase *tc_chain = tcase_create ("nestedtimelinepreload");

  suite_add_tcase (s, tc_chain);

  tcase_add_test (tc_chain, test_gessrc_posts_new_timeline_message);

  return s;
}

GST_CHECK_MAIN (ges);
