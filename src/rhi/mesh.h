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

#ifndef PEABERRY_RHI_MESH_H
#define PEABERRY_RHI_MESH_H

#include "peaberry/peaberry.h"
#include "pbr/vertex.h"
#include "rhi/buffer.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct pb_rhi_mesh {
    pb_rhi_buffer vertices;
    pb_rhi_buffer indices;
    uint32_t index_count;
    VkIndexType index_type;
} pb_rhi_mesh;

typedef struct pb_rhi_mesh_uv_sphere_desc {
    float radius;
    uint32_t sectors;
    uint32_t stacks;
} pb_rhi_mesh_uv_sphere_desc;

bool pb_rhi_mesh_create_interleaved(
    pb_context *context,
    const pb_pbr_vertex *vertices,
    uint32_t vertex_count,
    const void *indices,
    uint32_t index_count,
    VkIndexType index_type,
    pb_rhi_mesh *mesh);

bool pb_rhi_mesh_create_uv_sphere(
    pb_context *context,
    const pb_rhi_mesh_uv_sphere_desc *desc,
    pb_rhi_mesh *mesh);
void pb_rhi_mesh_destroy(pb_context *context, pb_rhi_mesh *mesh);

#endif
