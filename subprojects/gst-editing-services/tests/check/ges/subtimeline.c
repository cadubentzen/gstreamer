/* GStreamer Editing Services
 *
 * Copyright (C) 2024 GStreamer developers
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

#include "ges/ges-enums.h"
#include "ges/ges-layer.h"
#include "ges/ges-timeline.h"
#include "gst/check/gstcheck.h"
#include "test-utils.h"
#include <ges/ges.h>

#define CHECK_LAYER_CLIP_COUNT(layer_var, expected_count, layer_name) \
{ \
  GList *clips = ges_layer_get_clips (layer_var); \
  fail_unless (g_list_length (clips) == expected_count, \
      "%s should have %d clip(s) after operation, has %d", \
      layer_name, expected_count, g_list_length (clips)); \
  g_list_free_full (clips, gst_object_unref); \
}

#define CHECK_LAYER_CLIP_COUNTS_2(layer1_var, expected1, layer1_name, layer2_var, expected2, layer2_name) \
{ \
  GList *layer1_clips = ges_layer_get_clips (layer1_var); \
  GList *layer2_clips = ges_layer_get_clips (layer2_var); \
  fail_unless (g_list_length (layer1_clips) == expected1, \
      "%s should have %d clip(s) after operation, has %d", \
      layer1_name, expected1, g_list_length (layer1_clips)); \
  fail_unless (g_list_length (layer2_clips) == expected2, \
      "%s should have %d clip(s) after operation, has %d", \
      layer2_name, expected2, g_list_length (layer2_clips)); \
  g_list_free_full (layer1_clips, gst_object_unref); \
  g_list_free_full (layer2_clips, gst_object_unref); \
}

GST_START_TEST (test_subtimeline_primary_registration)
{
  GESTimeline *timeline;
  GError *error = NULL;

  ges_init ();

  /* Create a test timeline */
  timeline = ges_timeline_new ();
  fail_unless (timeline != NULL);

  /* Add a video track */
  GESTrack *track = GES_TRACK (ges_video_track_new ());
  fail_unless (ges_timeline_add_track (timeline, track));

  /* Add a layer */
  GESLayer *layer = ges_layer_new ();
  fail_unless (ges_timeline_add_layer (timeline, layer));

  /* Register the timeline as a primary */
  const gchar *primary_id = "test_primary_1";
  fail_unless (ges_timeline_register_as_subtimeline_primary (timeline,
          primary_id, &error), "Failed to register timeline as primary: %s",
      error ? error->message : "unknown error");
  fail_unless (error == NULL);

  /* Try to register with same ID (should fail) */
  fail_if (ges_timeline_register_as_subtimeline_primary (timeline,
          primary_id, &error),
      "Should not be able to register duplicate primary ID");
  fail_unless (error != NULL);
  g_clear_error (&error);

  /* Unregister the primary */
  fail_unless (ges_timeline_unregister_as_subtimeline_primary (primary_id,
          &error), "Failed to unregister primary: %s",
      error ? error->message : "unknown error");
  fail_unless (error == NULL);

  /* Try to unregister non-existent primary (should fail) */
  fail_if (ges_timeline_unregister_as_subtimeline_primary ("non_existent",
          &error), "Should not be able to unregister non-existent primary");
  fail_unless (error != NULL);
  g_clear_error (&error);

  gst_object_unref (timeline);
  ges_deinit ();
}

GST_END_TEST;

GST_START_TEST (test_subtimeline_replica_creation)
{
  GESTimeline *primary_timeline, *replica_timeline;
  GError *error = NULL;

  ges_init ();

  /* Create and setup primary timeline */
  primary_timeline = ges_timeline_new ();
  fail_unless (primary_timeline != NULL);

  /* Add tracks */
  GESTrack *video_track = GES_TRACK (ges_video_track_new ());
  GESTrack *audio_track = GES_TRACK (ges_audio_track_new ());
  fail_unless (ges_timeline_add_track (primary_timeline, video_track));
  fail_unless (ges_timeline_add_track (primary_timeline, audio_track));

  /* Add layers */
  GESLayer *layer1 = ges_layer_new ();
  GESLayer *layer2 = ges_layer_new ();
  ges_timeline_add_layer (primary_timeline, layer1);
  ges_timeline_add_layer (primary_timeline, layer2);

  /* Set timeline properties */
  ges_timeline_set_auto_transition (primary_timeline, TRUE);
  ges_timeline_set_snapping_distance (primary_timeline, 10 * GST_MSECOND);

  /* Register as primary with unique ID */
  const gchar *primary_id = "test_primary_2";
  fail_unless (ges_timeline_register_as_subtimeline_primary (primary_timeline,
          primary_id, &error), "Failed to register primary: %s",
      error ? error->message : "unknown error");

  /* Create replica from primary using subtimeline URI */
  gchar *subtimeline_uri = g_strdup_printf ("gessubtimeline:%s", primary_id);
  replica_timeline = ges_timeline_new_from_uri (subtimeline_uri, &error);
  fail_unless (replica_timeline != NULL,
      "Failed to create replica from primary: %s",
      error ? error->message : "unknown error");

  /* Verify replica has same structure as primary */
  GList *primary_tracks = ges_timeline_get_tracks (primary_timeline);
  GList *replica_tracks = ges_timeline_get_tracks (replica_timeline);
  fail_unless_equals_int (g_list_length (primary_tracks),
      g_list_length (replica_tracks));

  GList *primary_layers = ges_timeline_get_layers (primary_timeline);
  GList *replica_layers = ges_timeline_get_layers (replica_timeline);
  fail_unless (g_list_length (primary_layers) == g_list_length (replica_layers),
      "Instance should have same number of layers as primary");

  /* Verify editing features are disabled on replica */
  fail_unless (ges_timeline_get_auto_transition (replica_timeline) == FALSE,
      "Auto-transition should be disabled on replica");

  /* Note: We don't copy snapping distance or other editing properties
   * since editing APIs are disabled on replica timelines */

  /* Clean up */
  g_list_free_full (primary_tracks, gst_object_unref);
  g_list_free_full (replica_tracks, gst_object_unref);
  g_list_free (primary_layers);
  g_list_free (replica_layers);

  /* Unregister primary */
  ASSERT_OBJECT_REFCOUNT (primary_timeline, "primary_timeline", 2);
  fail_unless (ges_timeline_unregister_as_subtimeline_primary (primary_id,
          &error), "Failed to unregister primary: %s", error->message);

  g_free (subtimeline_uri);
  gst_object_unref (primary_timeline);
  gst_object_unref (replica_timeline);

  ges_deinit ();
}

GST_END_TEST;

GST_START_TEST (test_subtimeline_formatter)
{
  GESTimeline *primary_timeline, *test_timeline;
  GError *error = NULL;
  gboolean ret;

  ges_init ();

  /* Create primary timeline */
  primary_timeline = ges_timeline_new ();

  /* Add content */
  GESTrack *track = GES_TRACK (ges_video_track_new ());
  ges_timeline_add_track (primary_timeline, track);

  GESLayer *layer = ges_layer_new ();
  ges_timeline_add_layer (primary_timeline, layer);

  /* Add a test clip */
  GESTestClip *clip = ges_test_clip_new ();
  ges_timeline_element_set_start (GES_TIMELINE_ELEMENT (clip), 0);
  ges_timeline_element_set_duration (GES_TIMELINE_ELEMENT (clip),
      5 * GST_SECOND);
  ges_layer_add_clip (layer, GES_CLIP (clip));

  /* Register as primary with unique ID */
  const gchar *primary_id = "test_primary_3";
  ret =
      ges_timeline_register_as_subtimeline_primary (primary_timeline,
      primary_id, &error);
  fail_unless (ret == TRUE);
  fail_unless (error == NULL);

  /* Test SubtimelineFormatter directly */
  GESAsset *formatter_asset = ges_asset_request (GES_TYPE_FORMATTER,
      "GESSubTimelineFormatter", &error);
  fail_unless (formatter_asset != NULL,
      "Failed to get SubTimelineFormatter asset: %s",
      error ? error->message : "unknown error");
  fail_unless (error == NULL);

  GESFormatter *formatter =
      GES_FORMATTER (ges_asset_extract (formatter_asset, &error));
  GST_ERROR_OBJECT (formatter, "Yes");
  fail_unless (formatter != NULL, "Failed to create SubTimelineFormatter: %s",
      error ? error->message : "unknown error");
  fail_unless (error == NULL);

  test_timeline = ges_timeline_new ();
  gchar *subtimeline_uri = g_strdup_printf ("gessubtimeline:%s", primary_id);

  /* Test can_load_uri */
  ret = ges_formatter_can_load_uri (subtimeline_uri, &error);
  fail_unless (ret == TRUE,
      "Formatter should be able to load subtimeline URI: %s",
      error ? error->message : "unknown error");
  fail_unless (error == NULL);

  /* Test load_from_uri */
  ret =
      ges_formatter_load_from_uri (formatter, test_timeline, subtimeline_uri,
      &error);
  fail_unless (ret == TRUE, "Failed to load timeline using formatter: %s",
      error ? error->message : "unknown error");
  fail_unless (error == NULL);

  /* Verify content was copied */
  GList *layers = ges_timeline_get_layers (test_timeline);
  fail_unless (g_list_length (layers) == 1,
      "Loaded timeline should have one layer");

  if (layers) {
    GESLayer *loaded_layer = GES_LAYER (layers->data);
    CHECK_LAYER_CLIP_COUNT (loaded_layer, 1, "Loaded layer");
    GList *clips = ges_layer_get_clips (loaded_layer);
    if (clips) {
      GESClip *loaded_clip = GES_CLIP (clips->data);
      fail_unless (_DURATION (loaded_clip) == 5 * GST_SECOND,
          "Clip duration should be preserved");
    }

    g_list_free_full (clips, gst_object_unref);
  }

  g_list_free (layers);

  /* Clean up */
  fail_unless (ges_timeline_unregister_as_subtimeline_primary (primary_id,
          &error), "Failed to unregister primary during cleanup: %s",
      error->message);
  g_free (subtimeline_uri);
  gst_object_unref (formatter);
  gst_object_unref (primary_timeline);
  gst_object_unref (test_timeline);
}

GST_END_TEST;

GST_START_TEST (test_subtimeline_primary_status_apis)
{
  GESTimeline *timeline1, *timeline2;
  gchar *primary_id;
  gboolean result;

  ges_init ();

  /* Create test timelines */
  timeline1 = ges_timeline_new ();
  timeline2 = ges_timeline_new ();

  /* Initially, neither timeline should be a primary */
  fail_unless (ges_timeline_is_subtimeline_primary (timeline1) == FALSE,
      "Timeline should not be a primary initially");
  fail_unless (ges_timeline_is_subtimeline_primary (timeline2) == FALSE,
      "Timeline should not be a primary initially");

  /* Initially, both should return NULL for primary ID */
  primary_id = ges_timeline_get_subtimeline_primary_id (timeline1);
  fail_unless (primary_id == NULL,
      "Timeline should have no primary ID initially");
  primary_id = ges_timeline_get_subtimeline_primary_id (timeline2);
  fail_unless (primary_id == NULL,
      "Timeline should have no primary ID initially");

  /* Register timeline1 as a primary */
  result =
      ges_timeline_register_as_subtimeline_primary (timeline1,
      "test_primary_api", NULL);
  fail_unless (result == TRUE, "Should successfully register primary");

  /* Now timeline1 should be a primary */
  fail_unless (ges_timeline_is_subtimeline_primary (timeline1) == TRUE,
      "Timeline1 should be a primary after registration");
  fail_unless (ges_timeline_is_subtimeline_primary (timeline2) == FALSE,
      "Timeline2 should still not be a primary");

  /* timeline1 should return the correct primary ID */
  primary_id = ges_timeline_get_subtimeline_primary_id (timeline1);
  fail_unless (primary_id != NULL, "Timeline1 should have a primary ID");
  fail_unless (g_strcmp0 (primary_id, "test_primary_api") == 0,
      "Timeline1 should return correct primary ID");
  g_free (primary_id);

  /* timeline2 should still return NULL */
  primary_id = ges_timeline_get_subtimeline_primary_id (timeline2);
  fail_unless (primary_id == NULL, "Timeline2 should still have no primary ID");

  /* Unregister the primary */
  GError *unregister_error = NULL;
  result =
      ges_timeline_unregister_as_subtimeline_primary ("test_primary_api",
      &unregister_error);
  fail_unless (result == TRUE, "Should successfully unregister primary: %s",
      unregister_error->message);

  /* After unregistration, timeline1 should no longer be a primary */
  fail_unless (ges_timeline_is_subtimeline_primary (timeline1) == FALSE,
      "Timeline1 should not be a primary after unregistration");

  /* timeline1 should return NULL for primary ID after unregistration */
  primary_id = ges_timeline_get_subtimeline_primary_id (timeline1);
  fail_unless (primary_id == NULL,
      "Timeline1 should have no primary ID after unregistration");

  /* Clean up */
  gst_object_unref (timeline1);
  gst_object_unref (timeline2);

  ges_deinit ();
}

GST_END_TEST;

GST_START_TEST (test_subtimeline_property_propagation)
{
  GESTimeline *primary_timeline, *replica_timeline;
  GError *error = NULL;
  gboolean ret;

  ges_init ();

  /* Create and setup primary timeline */
  primary_timeline = ges_timeline_new ();
  fail_unless (primary_timeline != NULL);

  /* Add tracks and layers */
  GESTrack *video_track = GES_TRACK (ges_video_track_new ());
  ges_timeline_add_track (primary_timeline, video_track);

  GESLayer *layer = ges_layer_new ();
  ges_timeline_add_layer (primary_timeline, layer);

  /* Add a test clip */
  GESTestClip *clip = ges_test_clip_new ();
  ges_timeline_element_set_start (GES_TIMELINE_ELEMENT (clip), 0);
  ges_timeline_element_set_duration (GES_TIMELINE_ELEMENT (clip),
      5 * GST_SECOND);
  ges_layer_add_clip (layer, GES_CLIP (clip));

  /* Register as primary */
  const gchar *primary_id = "test_primary_prop";
  ret =
      ges_timeline_register_as_subtimeline_primary (primary_timeline,
      primary_id, &error);
  fail_unless (ret == TRUE, "Failed to register primary: %s",
      error ? error->message : "unknown error");
  fail_unless (error == NULL);

  /* Create replica */
  gchar *subtimeline_uri = g_strdup_printf ("gessubtimeline:%s", primary_id);
  replica_timeline = ges_timeline_new_from_uri (subtimeline_uri, &error);
  fail_unless (replica_timeline != NULL, "Failed to create replica: %s",
      error ? error->message : "unknown error");
  fail_unless (error == NULL);

  /* Get the clip from replica */
  GList *replica_layers = ges_timeline_get_layers (replica_timeline);
  fail_unless (replica_layers != NULL && g_list_length (replica_layers) == 1);
  GESLayer *replica_layer = GES_LAYER (replica_layers->data);

  CHECK_LAYER_CLIP_COUNT (replica_layer, 1, "Replica layer");
  GList *replica_clips = ges_layer_get_clips (replica_layer);
  GESClip *replica_clip = GES_CLIP (replica_clips->data);

  /* Test property propagation - change start time */
  GstClockTime new_start = 2 * GST_SECOND;
  ges_timeline_element_set_start (GES_TIMELINE_ELEMENT (clip), new_start);

  /* Verify the change was propagated (start changed, duration=5s, inpoint=0) */
  CHECK_OBJECT_PROPS (replica_clip, new_start, 0, 5 * GST_SECOND);

  /* Test property propagation - change duration */
  GstClockTime new_duration = 10 * GST_SECOND;
  ges_timeline_element_set_duration (GES_TIMELINE_ELEMENT (clip), new_duration);

  /* Verify the change was propagated (start=2s, duration changed, inpoint=0) */
  CHECK_OBJECT_PROPS (replica_clip, new_start, 0, new_duration);

  /* Test property propagation - change inpoint */
  GstClockTime new_inpoint = 1 * GST_SECOND;
  ges_timeline_element_set_inpoint (GES_TIMELINE_ELEMENT (clip), new_inpoint);

  /* Verify all changes were propagated (start=2s, duration=10s, inpoint=1s) */
  CHECK_OBJECT_PROPS (replica_clip, new_start, new_inpoint, new_duration);

  /* Cleanup */
  g_list_free_full (replica_clips, gst_object_unref);
  g_list_free (replica_layers);
  fail_unless (ges_timeline_unregister_as_subtimeline_primary (primary_id,
          &error), "Failed to unregister primary during cleanup: %s",
      error->message);
  g_free (subtimeline_uri);
  gst_object_unref (primary_timeline);
  gst_object_unref (replica_timeline);
}

GST_END_TEST;

GST_START_TEST (test_subtimeline_clip_movement)
{
  GESTimeline *primary_timeline, *replica_timeline;
  GError *error = NULL;

  ges_init ();

  /* Create primary timeline with multiple layers */
  primary_timeline = ges_timeline_new ();
  fail_unless (primary_timeline != NULL);

  GESTrack *video_track = GES_TRACK (ges_video_track_new ());
  ges_timeline_add_track (primary_timeline, video_track);

  GESLayer *layer1 = ges_timeline_append_layer (primary_timeline);
  GESLayer *layer2 = ges_timeline_append_layer (primary_timeline);

  /* Add test clips to first layer */
  GESAsset *testasset = ges_asset_request (GES_TYPE_TEST_CLIP, NULL, NULL);
  GESClip *clip1 = ges_layer_add_asset (layer1, testasset, 0, 0, 3,
      GES_TRACK_TYPE_UNKNOWN);
  GESClip *clip2 = ges_layer_add_asset (layer1, testasset, 5, 0,
      3, GES_TRACK_TYPE_UNKNOWN);

  CHECK_LAYER_CLIP_COUNTS_2 (layer1, 2, "Layer1", layer2, 0, "Layer2");

  /* Register as primary */
  const gchar *primary_id = "test_primary_movement";
  fail_unless (ges_timeline_register_as_subtimeline_primary (primary_timeline,
          primary_id, &error), "Failed to register primary: %s",
      error ? error->message : "unknown error");

  /* Create replica */
  gchar *subtimeline_uri = g_strdup_printf ("gessubtimeline:%s", primary_id);
  replica_timeline = ges_timeline_new_from_uri (subtimeline_uri, &error);
  fail_unless (replica_timeline != NULL);
  fail_unless (error == NULL);

  /* Verify initial state */
  GList *replica_layers = ges_timeline_get_layers (replica_timeline);
  fail_unless (g_list_length (replica_layers) == 2,
      "Should have 2 replica layers");

  /* Test moving clip to different layer */
  fail_unless (ges_clip_move_to_layer (GES_CLIP (clip1), layer2),
      "Should be able to move clip to layer2");

  CHECK_LAYER_CLIP_COUNTS_2 (layer1, 1, "Layer1", layer2, 1, "Layer2");

  /* Verify the clip was moved in replica as well */
  GESLayer *replica_layer1 = g_list_nth_data (replica_layers, 0);
  GESLayer *replica_layer2 = g_list_nth_data (replica_layers, 1);

  CHECK_LAYER_CLIP_COUNTS_2 (replica_layer1, 1, "Replica Layer1",
      replica_layer2, 1, "Replica Layer2");

  GList *replica_layer1_clips = ges_layer_get_clips (replica_layer1);
  GList *replica_layer2_clips = ges_layer_get_clips (replica_layer2);
  /* Test multiple property changes in sequence */
  ges_timeline_element_set_start (GES_TIMELINE_ELEMENT (clip2), 10);
  ges_timeline_element_set_duration (GES_TIMELINE_ELEMENT (clip2), 5);
  CHECK_OBJECT_PROPS (replica_layer1_clips->data, 10, 0, 5);

  /* Cleanup */
  g_list_free_full (replica_layer1_clips, gst_object_unref);
  g_list_free_full (replica_layer2_clips, gst_object_unref);
  g_list_free (replica_layers);
  fail_unless (ges_timeline_unregister_as_subtimeline_primary (primary_id,
          &error), "Failed to unregister primary during cleanup: %s",
      error->message);
  g_free (subtimeline_uri);
  gst_object_unref (primary_timeline);
  gst_object_unref (replica_timeline);
}

GST_END_TEST;
GST_START_TEST (test_subtimeline_dynamic_clip_operations)
{
  GESTimeline *primary_timeline, *replica_timeline;
  GError *error = NULL;
  ges_init ();
  /* Create primary timeline */
  primary_timeline = ges_timeline_new ();
  fail_unless (primary_timeline != NULL);
  GESTrack *video_track = GES_TRACK (ges_video_track_new ());
  ges_timeline_add_track (primary_timeline, video_track);
  GESLayer *layer = ges_layer_new ();
  ges_timeline_add_layer (primary_timeline, layer);
  /* Register as primary */
  const gchar *primary_id = "test_primary_dynamic";

  fail_unless (ges_timeline_register_as_subtimeline_primary (primary_timeline,
          primary_id, &error), "Failed to register primary: %s",
      error->message);

  /* Create replica */
  gchar *subtimeline_uri = g_strdup_printf ("gessubtimeline:%s", primary_id);
  replica_timeline = ges_timeline_new_from_uri (subtimeline_uri, &error);
  fail_unless (replica_timeline, "Failed to create replica: %s",
      error->message);

  /* Test adding clips dynamically after replica creation */
  GESTestClip *clip1 = ges_test_clip_new ();
  ges_timeline_element_set_start (GES_TIMELINE_ELEMENT (clip1), 0);
  ges_timeline_element_set_duration (GES_TIMELINE_ELEMENT (clip1),
      2 * GST_SECOND);
  ges_layer_add_clip (layer, GES_CLIP (clip1));
  assert_num_children (clip1, 1);

  /* Verify clip was added to replica */
  GList *replica_layers = ges_timeline_get_layers (replica_timeline);
  GESLayer *replica_layer = GES_LAYER (replica_layers->data);
  CHECK_LAYER_CLIP_COUNT (replica_layer, 1,
      "Replica layer after dynamic addition");
  GList *replica_clips = ges_layer_get_clips (replica_layer);
  GST_ERROR ("%" GES_FORMAT, GES_ARGS (clip1));
  assert_num_children (clip1, 1);
  assert_num_children (replica_clips->data, 1);

  /* Test adding another clip */
  GESTestClip *clip2 = ges_test_clip_new ();
  ges_timeline_element_set_start (GES_TIMELINE_ELEMENT (clip2), 3 * GST_SECOND);
  ges_timeline_element_set_duration (GES_TIMELINE_ELEMENT (clip2),
      2 * GST_SECOND);
  ges_layer_add_clip (layer, GES_CLIP (clip2));
  g_list_free_full (replica_clips, gst_object_unref);
  CHECK_LAYER_CLIP_COUNT (replica_layer, 2,
      "Replica layer after second addition");
  replica_clips = ges_layer_get_clips (replica_layer);
  /* Test removing a clip dynamically */
  ges_layer_remove_clip (layer, GES_CLIP (clip1));
  g_list_free_full (replica_clips, gst_object_unref);
  replica_clips = ges_layer_get_clips (replica_layer);
  fail_unless (g_list_length (replica_clips) == 1,
      "Replica should have 1 clip after removal");
  /* Test modifying remaining clip properties */
  ges_timeline_element_set_start (GES_TIMELINE_ELEMENT (clip2), 1 * GST_SECOND);
  ges_timeline_element_set_duration (GES_TIMELINE_ELEMENT (clip2),
      4 * GST_SECOND);
  GESClip *remaining_replica_clip = GES_CLIP (replica_clips->data);
  fail_unless (_START (remaining_replica_clip) == 1 * GST_SECOND);
  fail_unless (_DURATION (remaining_replica_clip) == 4 * GST_SECOND);
  /* Cleanup */
  g_list_free_full (replica_clips, gst_object_unref);
  g_list_free (replica_layers);
  fail_unless (ges_timeline_unregister_as_subtimeline_primary (primary_id,
          &error), "Failed to unregister primary during cleanup: %s",
      error->message);
  g_free (subtimeline_uri);
  gst_object_unref (primary_timeline);
  gst_object_unref (replica_timeline);
}

GST_END_TEST;
static Suite *
ges_suite (void)
{
  Suite *s = suite_create ("ges-subtimeline");
  TCase *tc_chain = tcase_create ("subtimeline");
  suite_add_tcase (s, tc_chain);
  tcase_add_test (tc_chain, test_subtimeline_primary_registration);
  tcase_add_test (tc_chain, test_subtimeline_replica_creation);
  tcase_add_test (tc_chain, test_subtimeline_formatter);
  tcase_add_test (tc_chain, test_subtimeline_primary_status_apis);
  tcase_add_test (tc_chain, test_subtimeline_property_propagation);
  tcase_add_test (tc_chain, test_subtimeline_clip_movement);
  tcase_add_test (tc_chain, test_subtimeline_dynamic_clip_operations);
  return s;
}

GST_CHECK_MAIN (ges);
