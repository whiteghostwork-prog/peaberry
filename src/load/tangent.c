/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "load/tangent.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read_index(const void *indices, uint32_t tri, VkIndexType type)
{
    if (type == VK_INDEX_TYPE_UINT16) {
        const uint16_t *idx = indices;
        return idx[tri];
    }

    const uint32_t *idx = indices;
    return idx[tri];
}

void pb_pbr_generate_tangents(
    pb_pbr_vertex *vertices,
    uint32_t vertex_count,
    const void *indices,
    uint32_t index_count,
    VkIndexType index_type)
{
    if (!vertices || vertex_count == 0 || !indices || index_count < 3) {
        return;
    }

    float *tan1 = calloc(vertex_count * 3, sizeof(float));
    float *tan2 = calloc(vertex_count * 3, sizeof(float));
    if (!tan1 || !tan2) {
        free(tan1);
        free(tan2);
        return;
    }

    for (uint32_t tri = 0; tri + 2 < index_count; tri += 3) {
        const uint32_t i0 = read_index(indices, tri, index_type);
        const uint32_t i1 = read_index(indices, tri + 1, index_type);
        const uint32_t i2 = read_index(indices, tri + 2, index_type);

        if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) {
            continue;
        }

        const pb_pbr_vertex *v0 = &vertices[i0];
        const pb_pbr_vertex *v1 = &vertices[i1];
        const pb_pbr_vertex *v2 = &vertices[i2];

        const float x1 = v1->pos[0] - v0->pos[0];
        const float y1 = v1->pos[1] - v0->pos[1];
        const float z1 = v1->pos[2] - v0->pos[2];
        const float x2 = v2->pos[0] - v0->pos[0];
        const float y2 = v2->pos[1] - v0->pos[1];
        const float z2 = v2->pos[2] - v0->pos[2];

        const float s1 = v1->uv[0] - v0->uv[0];
        const float t1 = v1->uv[1] - v0->uv[1];
        const float s2 = v2->uv[0] - v0->uv[0];
        const float t2 = v2->uv[1] - v0->uv[1];

        const float denom = s1 * t2 - s2 * t1;
        if (fabsf(denom) < 1e-8f) {
            continue;
        }

        const float r = 1.0f / denom;
        const float sdir[3] = {
            (t2 * x1 - t1 * x2) * r,
            (t2 * y1 - t1 * y2) * r,
            (t2 * z1 - t1 * z2) * r,
        };
        const float tdir[3] = {
            (s1 * x2 - s2 * x1) * r,
            (s1 * y2 - s2 * y1) * r,
            (s1 * z2 - s2 * z1) * r,
        };

        for (uint32_t corner = 0; corner < 3; ++corner) {
            const uint32_t idx = corner == 0 ? i0 : corner == 1 ? i1 : i2;
            tan1[idx * 3 + 0] += sdir[0];
            tan1[idx * 3 + 1] += sdir[1];
            tan1[idx * 3 + 2] += sdir[2];
            tan2[idx * 3 + 0] += tdir[0];
            tan2[idx * 3 + 1] += tdir[1];
            tan2[idx * 3 + 2] += tdir[2];
        }
    }

    for (uint32_t i = 0; i < vertex_count; ++i) {
        const float *n = vertices[i].normal;
        const float *t = &tan1[i * 3];

        float tangent[3] = { t[0], t[1], t[2] };
        const float ndot = tangent[0] * n[0] + tangent[1] * n[1] + tangent[2] * n[2];
        tangent[0] -= n[0] * ndot;
        tangent[1] -= n[1] * ndot;
        tangent[2] -= n[2] * ndot;

        float len = sqrtf(
            tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
        if (len < 1e-8f) {
            tangent[0] = 1.0f;
            tangent[1] = 0.0f;
            tangent[2] = 0.0f;
            len = 1.0f;
        }

        const float bit[3] = {
            n[1] * tangent[2] - n[2] * tangent[1],
            n[2] * tangent[0] - n[0] * tangent[2],
            n[0] * tangent[1] - n[1] * tangent[0],
        };
        const float bit_len = sqrtf(bit[0] * bit[0] + bit[1] * bit[1] + bit[2] * bit[2]);
        const float bit_dot = bit_len > 1e-8f
            ? (bit[0] * tan2[i * 3 + 0] + bit[1] * tan2[i * 3 + 1] + bit[2] * tan2[i * 3 + 2]) / bit_len
            : 1.0f;

        vertices[i].tangent[0] = tangent[0] / len;
        vertices[i].tangent[1] = tangent[1] / len;
        vertices[i].tangent[2] = tangent[2] / len;
        vertices[i].tangent[3] = bit_dot < 0.0f ? -1.0f : 1.0f;
    }

    free(tan1);
    free(tan2);
}
