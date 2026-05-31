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

#include "rhi/mesh.h"

#include "core/log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const float k_pi = 3.14159265358979323846f;

typedef struct pb_rhi_mesh_vertex {
    float pos[3];
    float normal[3];
    float uv[2];
    float tangent[4];
} pb_rhi_mesh_vertex;

static void mesh_reset(pb_rhi_mesh *mesh)
{
    *mesh = (pb_rhi_mesh){0};
}

bool pb_rhi_mesh_create_uv_sphere(
    pb_context *context,
    const pb_rhi_mesh_uv_sphere_desc *desc,
    pb_rhi_mesh *mesh)
{
    if (!context || !desc || !mesh || desc->radius <= 0.0f || desc->sectors < 3 || desc->stacks < 2) {
        return false;
    }

    mesh_reset(mesh);

    const uint32_t sectors = desc->sectors;
    const uint32_t stacks = desc->stacks;
    const uint32_t vertex_count = (stacks + 1) * (sectors + 1);
    const uint32_t index_capacity = stacks * sectors * 6;

    pb_rhi_mesh_vertex *vertices = calloc(vertex_count, sizeof(*vertices));
    uint32_t *indices = calloc(index_capacity, sizeof(*indices));
    if (!vertices || !indices) {
        free(vertices);
        free(indices);
        return false;
    }

    uint32_t vertex_index = 0;
    for (uint32_t stack = 0; stack <= stacks; ++stack) {
        const float stack_angle = (float)stack * k_pi / (float)stacks;
        const float xy = desc->radius * sinf(stack_angle);
        const float z = desc->radius * cosf(stack_angle);

        for (uint32_t sector = 0; sector <= sectors; ++sector) {
            const float sector_angle = (float)sector * 2.0f * k_pi / (float)sectors;
            const float x = xy * cosf(sector_angle);
            const float y = xy * sinf(sector_angle);

            pb_rhi_mesh_vertex *vertex = &vertices[vertex_index++];
            vertex->pos[0] = x;
            vertex->pos[1] = y;
            vertex->pos[2] = z;
            vertex->normal[0] = x / desc->radius;
            vertex->normal[1] = y / desc->radius;
            vertex->normal[2] = z / desc->radius;
            vertex->uv[0] = (float)sector / (float)sectors;
            vertex->uv[1] = (float)stack / (float)stacks;
            /* Tangent follows increasing U (sector); bitangent sign in w (Mikktspace). */
            vertex->tangent[0] = -sinf(sector_angle);
            vertex->tangent[1] = cosf(sector_angle);
            vertex->tangent[2] = 0.0f;
            vertex->tangent[3] = 1.0f;
        }
    }

    uint32_t index = 0;
    for (uint32_t stack = 0; stack < stacks; ++stack) {
        const uint32_t k1 = stack * (sectors + 1);
        const uint32_t k2 = k1 + sectors + 1;

        for (uint32_t sector = 0; sector < sectors; ++sector) {
            if (stack != 0) {
                indices[index++] = k1 + sector;
                indices[index++] = k2 + sector;
                indices[index++] = k1 + sector + 1;
            }

            if (stack != stacks - 1) {
                indices[index++] = k1 + sector + 1;
                indices[index++] = k2 + sector;
                indices[index++] = k2 + sector + 1;
            }
        }
    }

    pb_rhi_buffer_desc vertex_desc = {
        .size = vertex_count * sizeof(*vertices),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };

    pb_rhi_buffer_desc index_desc = {
        .size = index * sizeof(*indices),
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };

    bool ok = pb_rhi_buffer_create(context, &vertex_desc, &mesh->vertices) &&
              pb_rhi_buffer_upload(context, &mesh->vertices, vertices, vertex_desc.size) &&
              pb_rhi_buffer_create(context, &index_desc, &mesh->indices) &&
              pb_rhi_buffer_upload(context, &mesh->indices, indices, index_desc.size);

    mesh->index_count = index;

    free(vertices);
    free(indices);

    if (!ok) {
        pb_log_error("Failed to upload UV sphere mesh");
        pb_rhi_mesh_destroy(context, mesh);
        return false;
    }

    return true;
}

void pb_rhi_mesh_destroy(pb_context *context, pb_rhi_mesh *mesh)
{
    if (!context || !mesh) {
        return;
    }

    pb_rhi_buffer_destroy(context, &mesh->vertices);
    pb_rhi_buffer_destroy(context, &mesh->indices);
    mesh_reset(mesh);
}
