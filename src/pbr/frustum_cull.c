/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pbr/frustum_cull.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static void normalize_plane(float plane[4])
{
    const float length = sqrtf(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
    const float epsilon = 1e-6f;
    if (length < epsilon) {
        return;
    }

    plane[0] /= length;
    plane[1] /= length;
    plane[2] /= length;
    plane[3] /= length;
}

void pb_frustum_from_view_proj(const pb_mat4 view, const pb_mat4 proj, pb_frustum *out)
{
    pb_mat4 clip;
    pb_mat4_mul(proj, view, clip);

    for (int i = 0; i < 4; ++i) {
        out->planes[0][i] = clip[3][i] + clip[0][i];
        out->planes[1][i] = clip[3][i] - clip[0][i];
        out->planes[2][i] = clip[3][i] + clip[1][i];
        out->planes[3][i] = clip[3][i] - clip[1][i];
        out->planes[4][i] = clip[3][i] + clip[2][i];
        out->planes[5][i] = clip[3][i] - clip[2][i];
    }

    for (int p = 0; p < 6; ++p) {
        normalize_plane(out->planes[p]);
    }
}

static void world_point(const pb_mat4 world, const float local[3], float out[3])
{
    out[0] = world[0][0] * local[0] + world[1][0] * local[1] + world[2][0] * local[2] + world[3][0];
    out[1] = world[0][1] * local[0] + world[1][1] * local[1] + world[2][1] * local[2] + world[3][1];
    out[2] = world[0][2] * local[0] + world[1][2] * local[1] + world[2][2] * local[2] + world[3][2];
}

bool pb_frustum_intersects_bounds(
    const pb_frustum *frustum,
    const pb_mat4 world,
    const float bounds_min[3],
    const float bounds_max[3])
{
    if (!frustum) {
        return true;
    }

    float corners[8][3];
    for (uint32_t corner = 0; corner < 8; ++corner) {
        const float local[3] = {
            (corner & 1) ? bounds_max[0] : bounds_min[0],
            (corner & 2) ? bounds_max[1] : bounds_min[1],
            (corner & 4) ? bounds_max[2] : bounds_min[2],
        };
        world_point(world, local, corners[corner]);
    }

    for (int plane = 0; plane < 6; ++plane) {
        const float *p = frustum->planes[plane];
        bool outside = true;

        for (uint32_t corner = 0; corner < 8; ++corner) {
            const float distance =
                p[0] * corners[corner][0] + p[1] * corners[corner][1] + p[2] * corners[corner][2] + p[3];
            if (distance >= 0.0f) {
                outside = false;
                break;
            }
        }

        if (outside) {
            return false;
        }
    }

    return true;
}
