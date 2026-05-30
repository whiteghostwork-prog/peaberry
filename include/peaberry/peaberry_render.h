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

#ifndef PEABERRY_RENDER_H
#define PEABERRY_RENDER_H

#include "peaberry/peaberry_vk.h"

typedef struct pb_triangle_pass pb_triangle_pass;

typedef struct pb_triangle_pass_desc {
    pb_context *context;
    VkRenderPass render_pass;
    const char *vert_spv_path;
    const char *frag_spv_path;
} pb_triangle_pass_desc;

pb_triangle_pass *pb_triangle_pass_create(const pb_triangle_pass_desc *desc);
void pb_triangle_pass_destroy(pb_triangle_pass *pass);

void pb_triangle_pass_record(pb_triangle_pass *pass, VkCommandBuffer cmd, VkExtent2D extent);

#endif
