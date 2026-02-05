/* GStreamer Editing Services
 * Copyright (C) 2023 Thibault Saunier <tsaunier@igalia.com>
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
#include "ges-timeline.h"
#pragma once

typedef struct {
  GESClip *clip;
  GESTrack *track;
  /* Committed values captured during timeline commit - these reflect
   * what NLE sees, not potentially uncommitted GES edits */
  GstClockTime timeline_inpoint;    /* Where we start reading in the nested timeline */
  GstClockTime timeline_duration;   /* How much of the nested timeline we read */
  GstClockTime outer_start;         /* Where the clip appears in the outer timeline */
} NestedTimelineInfo;

typedef struct {
    GRecMutex lock;
    GArray *pooled_sources;
    GArray *prepared_sources;
    GESTimeline *timeline;
    GObject *pool;
    gboolean has_subtimelines;

    gboolean rendering;

    guint max_preloaded_sources;

    GArray *pending_nested_timelines;
    GPtrArray *pipeline_buses;
} GESPipelinePoolManager;

void ges_pipeline_pool_manager_init   (GESPipelinePoolManager *self, GESTimeline *timeline);
void ges_pipeline_pool_clear          (GESPipelinePoolManager *self);
void ges_pipeline_pool_manager_commit (GESPipelinePoolManager *self);

void ges_pipeline_pool_manager_prepare_pipelines_around (GESPipelinePoolManager *self,
                                                         GESTrack *track,
                                                         GstClockTime stack_start,
                                                         GstClockTime stack_stop);
void ges_pipeline_pool_manager_unprepare_all            (GESPipelinePoolManager *self);

void ges_pipeline_pool_manager_set_rendering (GESPipelinePoolManager * self, gboolean rendering);
