/*
 * Copyright 2026 The Peaberry Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PEABERRY_EXAMPLE_WSI_H
#define PEABERRY_EXAMPLE_WSI_H

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_bench.h"
#include "peaberry/peaberry_frame_metrics.h"
#include "peaberry/peaberry_vk.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct pb_example_wsi pb_example_wsi;

typedef struct pb_example_wsi_desc {
    pb_context *context;
    uint32_t width;
    uint32_t height;
    const char *title;
    bool enable_stats;
} pb_example_wsi_desc;

pb_example_wsi *pb_example_wsi_create(const pb_example_wsi_desc *desc);
void pb_example_wsi_destroy(pb_example_wsi *wsi);

void pb_example_wsi_poll(pb_example_wsi *wsi);
bool pb_example_wsi_should_close(const pb_example_wsi *wsi);
void *pb_example_wsi_window(const pb_example_wsi *wsi);

bool pb_example_wsi_begin_frame(pb_example_wsi *wsi, float r, float g, float b, float a);
bool pb_example_wsi_end_frame(pb_example_wsi *wsi);

VkRenderPass pb_example_wsi_render_pass(const pb_example_wsi *wsi);
VkExtent2D pb_example_wsi_extent(const pb_example_wsi *wsi);
VkCommandBuffer pb_example_wsi_command_buffer(const pb_example_wsi *wsi);

void pb_example_wsi_set_stats_enabled(pb_example_wsi *wsi, bool enabled);
bool pb_example_wsi_stats_enabled(const pb_example_wsi *wsi);
bool pb_example_wsi_last_metrics(const pb_example_wsi *wsi, pb_frame_metrics *out);
bool pb_example_wsi_update_stats_title(pb_example_wsi *wsi);

#endif
