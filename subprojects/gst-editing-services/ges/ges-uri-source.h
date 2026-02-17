/* GStreamer Editing Services
 * Copyright (C) 2020 Ubicast SAS
 *               Author: Thibault Saunier <tsaunier@igalia.com>
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

#pragma once

#include <glib-object.h>
#include <ges/ges.h>

G_BEGIN_DECLS

typedef struct _GESUriSource GESUriSource;

struct _GESUriSource
{
  GstElement *decodebin;        /* Reference owned by parent class */
  gchar *uri;

  GESTrackElement *element;

  GstPipeline *uridecodepool_pipeline;
  GWeakRef toplevel_pipeline;

  GList *parent_ges_uri_sources;
  gboolean controls_nested_timeline;
  gboolean disable_seek_in_ready;

  /* Seek event for positioning nested timeline sources during seek-in-ready.
   *
   * Flow: During _relink_single_node, the NLE composition sends a
   * translate-composition-seek signal BEFORE sync_state_with_parent.
   * For nested timeline sources (controls_nested_timeline=TRUE), the
   * translate callback stores the NLE-translated + time-effect-adjusted seek
   * here. Then when sync_state triggers start(), the get-initial-seek callback
   * returns this event to position the inner composition at the exact
   * sub-segment needed (e.g., [parent_inpoint, parent_inpoint+stack_duration]).
   *
   * Why we need this: Without it, the inner composition would start from
   * position 0, requiring a seek round-trip. With it, the inner composition
   * starts pre-positioned, avoiding the extra seek.
   *
   * Only used for nested timelines. Regular sources leave this NULL and use
   * the fallback path which computes the full clip range for pool reuse. */
  GstEvent *pending_seek_in_ready;
};

G_GNUC_INTERNAL gboolean      ges_uri_source_select_pad   (GESSource *self, GstPad *pad);
G_GNUC_INTERNAL GstElement *ges_uri_source_create_source  (GESUriSource *self);
G_GNUC_INTERNAL void         ges_uri_source_init          (GESTrackElement *element, GESUriSource *self);
G_GNUC_INTERNAL void         ges_uri_source_dispose      (GESUriSource *self);
G_GNUC_INTERNAL gboolean ges_source_uses_uridecodepoolsrc (GESSource * self);

G_END_DECLS
