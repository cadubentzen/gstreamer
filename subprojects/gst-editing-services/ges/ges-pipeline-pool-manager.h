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

typedef struct _GESPipelinePoolManager GESPipelinePoolManager;

GESPipelinePoolManager * ges_pipeline_pool_manager_new    (GESTimeline *timeline);
GESPipelinePoolManager * ges_pipeline_pool_manager_ref    (GESPipelinePoolManager *self);
void ges_pipeline_pool_manager_unref                      (GESPipelinePoolManager *self);
void ges_pipeline_pool_manager_commit                     (GESPipelinePoolManager *self);

void ges_pipeline_pool_manager_prepare_pipelines_around (GESPipelinePoolManager *self,
                                                         GESTrack *track,
                                                         GstClockTime stack_start,
                                                         GstClockTime stack_stop);
void ges_pipeline_pool_manager_unprepare_all            (GESPipelinePoolManager *self);

void ges_pipeline_pool_manager_set_rendering (GESPipelinePoolManager * self, gboolean rendering);
void ges_pipeline_pool_manager_set_max_preloaded_sources (GESPipelinePoolManager *self, guint max);
guint ges_pipeline_pool_manager_get_max_preloaded_sources (GESPipelinePoolManager *self);

void ges_pipeline_pool_manager_register_nested_timeline   (GESPipelinePoolManager *self,
                                                           GESTimeline *nested_timeline);
void ges_pipeline_pool_manager_deregister_nested_timeline (GESPipelinePoolManager *self,
                                                           GESTimeline *nested_timeline);

guint ges_pipeline_pool_manager_get_n_pooled_sources    (GESPipelinePoolManager *self);
guint ges_pipeline_pool_manager_get_n_prepared_sources  (GESPipelinePoolManager *self);
guint ges_pipeline_pool_manager_get_n_registered_nested (GESPipelinePoolManager *self);
