/* GStreamer
 * Copyright (C) 2013 Mathieu Duponchelle <mduponchelle1@gmail.com>
 * Copyright (C) 2013 Thibault Saunier <thibault.saunier@collabora.com>
 * Copyright (C) 2020 Thibault Saunier <tsaunier@igalia.com>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */

/**
 * SECTION: gesframecompositionmeta
 * @title: GESFrameCompositionMeta interface
 * @short_description: A Meta providing positioning information for a given
 * video frame
 *
 * Since: 1.24
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "ges-frame-composition-meta.h"
#include "gstframepositioner.h"
#include "ges-utils.h"

static gboolean ges_frame_composition_meta_init (GstMeta * meta,
    gpointer params, GstBuffer * buffer);
static gboolean ges_frame_composition_meta_transform (GstBuffer * dest,
    GstMeta * meta, GstBuffer * buffer, GQuark type, gpointer data);

GType
ges_frame_composition_meta_api_get_type (void)
{
  static GType type;
  static const gchar *tags[] = { "video", NULL };

  if (g_once_init_enter (&type)) {
    GType _type =
        gst_meta_api_type_register ("GESFrameCompositionMetaAPI", tags);
    g_once_init_leave (&type, _type);
  }
  return type;
}

static void
ges_frame_composition_meta_free (GstMeta * meta, GstBuffer * _buffer)
{
  GESFrameCompositionMeta *cmeta = (GESFrameCompositionMeta *) meta;

  gst_structure_set_parent_refcount (cmeta->extra_properties, NULL);
  gst_structure_free (cmeta->extra_properties);
  gst_clear_object (&cmeta->framepositioner);
}

static const GstMetaInfo *
ges_frame_composition_meta_get_info (void)
{
  static const GstMetaInfo *meta_info = NULL;

  if (g_once_init_enter ((GstMetaInfo **) & meta_info)) {
    const GstMetaInfo *meta =
        gst_meta_register (ges_frame_composition_meta_api_get_type (),
        "GESFrameCompositionMeta",
        sizeof (GESFrameCompositionMeta), ges_frame_composition_meta_init,
        ges_frame_composition_meta_free,
        ges_frame_composition_meta_transform);
    g_once_init_leave ((GstMetaInfo **) & meta_info, (GstMetaInfo *) meta);
  }
  return meta_info;
}

static gboolean
ges_frame_composition_meta_init (GstMeta * meta, gpointer params,
    GstBuffer * buffer)
{
  int default_operator_value = 0;
  GESFrameCompositionMeta *smeta;

  smeta = (GESFrameCompositionMeta *) meta;

  gst_compositor_operator_get_type_and_default_value (&default_operator_value);

  smeta->alpha = 0.0;
  smeta->posx = smeta->posy = smeta->height = smeta->width = 0;
  smeta->zorder = 0;
  smeta->operator = default_operator_value;
  smeta->extra_properties = NULL;

  return TRUE;
}

static gboolean
ges_frame_composition_meta_transform (GstBuffer * dest, GstMeta * meta,
    GstBuffer * buffer, GQuark type, gpointer data)
{
  GESFrameCompositionMeta *dmeta, *smeta;

  smeta = (GESFrameCompositionMeta *) meta;

  if (GST_META_TRANSFORM_IS_COPY (type)) {
    /* only copy if the complete data is copied as well */
    dmeta =
        (GESFrameCompositionMeta *) gst_buffer_add_meta (dest,
        ges_frame_composition_meta_get_info (), NULL);
    gst_structure_take (&dmeta->extra_properties,
        gst_structure_copy (smeta->extra_properties));
    gst_structure_set_parent_refcount (dmeta->extra_properties,
        &GST_MINI_OBJECT_REFCOUNT (dest));
    dmeta->alpha = smeta->alpha;
    dmeta->posx = smeta->posx;
    dmeta->posy = smeta->posy;
    dmeta->width = smeta->width;
    dmeta->height = smeta->height;
    dmeta->zorder = smeta->zorder;
    dmeta->operator = smeta->operator;
    if (smeta->framepositioner) {
      dmeta->framepositioner = gst_object_ref (smeta->framepositioner);
    } else {
      dmeta->framepositioner = NULL;
    }
  }

  return TRUE;
}

/**
 * ges_buffer_add_frame_composition_meta:
 * @buffer: #GstBuffer to which protection metadata should be added.
 *
 * Attaches positioning metadata to a #GstBuffer.
 *
 * Returns: (transfer none): a pointer to the added #GESFrameCompositionMeta.
 *
 * Since: 1.24
 */
GESFrameCompositionMeta *
ges_buffer_add_frame_composition_meta (GstBuffer * buffer)
{
  GESFrameCompositionMeta *meta;

  meta =
      (GESFrameCompositionMeta *) gst_buffer_add_meta (buffer,
      ges_frame_composition_meta_get_info (), NULL);
  return meta;
}

/**
 * ges_frame_composition_get_synced_meta:
 * @sinkpad: The sinkpad that received the buffer
 * @segment: #GstSegment containing timing information
 * @buf: #GstBuffer containing the meta
 *
 * Gets and synchronizes the frame composition meta with right framepositioner values for the
 * given sinkpad, segment and buffer PTS.
 *
 * This method ensures that the synchronization can happen on any element inside an
 * nlecomposition while running. This is necessary to handle time effects (rate changes,
 * time remapping) and clips with non-zero start times, ensuring keyframes are evaluated
 * at the correct media time scale.
 *
 * Returns: (transfer none) (nullable): The synchronized #GESFrameCompositionMeta, or %NULL if no meta found.
 *
 * Since: 1.28
 */
GESFrameCompositionMeta *
ges_frame_composition_get_synced_meta (GstPad * sinkpad,
    const GstSegment * segment, GstBuffer * buf)
{
  GstClockTime stream_time;

  g_return_val_if_fail (GST_IS_PAD (sinkpad), NULL);
  g_return_val_if_fail (segment != NULL, NULL);
  g_return_val_if_fail (buf != NULL, NULL);

  /* Convert segment time from object time scale to media time scale */
  if (!ges_nle_source_stream_time (sinkpad, segment, GST_BUFFER_PTS (buf),
          &stream_time)) {
    /* Fallback to original calculation if query fails */
    stream_time =
        gst_segment_to_stream_time (segment, GST_FORMAT_TIME,
        GST_BUFFER_PTS (buf));
    GST_DEBUG_OBJECT (sinkpad,
        "Time conversion query failed, using object time scale stream time %"
        GST_TIME_FORMAT, GST_TIME_ARGS (stream_time));
  } else {
    GST_DEBUG_OBJECT (sinkpad,
        "Using media time scale stream time %" GST_TIME_FORMAT,
        GST_TIME_ARGS (stream_time));
  }

  return gst_frame_positioner_sync_meta_internal (stream_time, buf);
}
