/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pbr/draw_sort.h"

#include <math.h>
#include <string.h>

#define CGLM_FORCE_DEPTH_ZERO_TO_ONE
#define CGLM_FORCE_LEFT_HANDED
#include <cglm/cglm.h>

static float view_space_z(
    const pb_mat4 view_model,
    float x,
    float y,
    float z)
{
    return view_model[0][2] * x + view_model[1][2] * y + view_model[2][2] * z + view_model[3][2];
}

float pb_draw_sort_view_depth(const pb_mat4 view, const pb_mat4 model)
{
    pb_mat4 view_copy;
    pb_mat4 model_copy;
    pb_mat4 view_model;

    memcpy(view_copy, view, sizeof(view_copy));
    memcpy(model_copy, model, sizeof(model_copy));
    pb_mat4_mul(view_copy, model_copy, view_model);
    return view_model[3][2];
}

float pb_draw_sort_view_depth_bounds(
    const pb_mat4 view,
    const pb_mat4 model,
    const float bounds_min[3],
    const float bounds_max[3],
    bool use_far_point)
{
    pb_mat4 view_copy;
    pb_mat4 model_copy;
    pb_mat4 view_model;

    memcpy(view_copy, view, sizeof(view_copy));
    memcpy(model_copy, model, sizeof(model_copy));
    pb_mat4_mul(view_copy, model_copy, view_model);

    float depth = use_far_point ? -INFINITY : INFINITY;
    for (uint32_t corner = 0; corner < 8; ++corner) {
        const float x = (corner & 1) ? bounds_max[0] : bounds_min[0];
        const float y = (corner & 2) ? bounds_max[1] : bounds_min[1];
        const float z = (corner & 4) ? bounds_max[2] : bounds_min[2];
        const float view_z = view_space_z(view_model, x, y, z);

        if (use_far_point) {
            depth = fmaxf(depth, view_z);
        } else {
            depth = fminf(depth, view_z);
        }
    }

    return depth;
}

float pb_draw_sort_blend_distance(
    const float camera_pos[3],
    const pb_mat4 model,
    const float bounds_min[3],
    const float bounds_max[3])
{
    mat4 model_mat;
    mat4 inv_model;
    memcpy(model_mat, model, sizeof(model_mat));
    glm_mat4_inv(model_mat, inv_model);

    vec3 camera_vec = { camera_pos[0], camera_pos[1], camera_pos[2] };
    vec3 camera_model;
    glm_mat4_mulv3(inv_model, camera_vec, 1.0f, camera_model);

    vec3 closest_model = {
        glm_clamp(camera_model[0], bounds_min[0], bounds_max[0]),
        glm_clamp(camera_model[1], bounds_min[1], bounds_max[1]),
        glm_clamp(camera_model[2], bounds_min[2], bounds_max[2]),
    };

    vec3 closest_world;
    glm_mat4_mulv3(model_mat, closest_model, 1.0f, closest_world);

    vec3 delta = {
        closest_world[0] - camera_vec[0],
        closest_world[1] - camera_vec[1],
        closest_world[2] - camera_vec[2],
    };
    return glm_vec3_norm(delta);
}

void pb_draw_sort_stable(
    pb_draw_sort_entry *entries,
    uint32_t count,
    bool back_to_front)
{
    if (!entries || count < 2) {
        return;
    }

    for (uint32_t i = 1; i < count; ++i) {
        const pb_draw_sort_entry key = entries[i];
        int32_t j = (int32_t)i - 1;

        while (j >= 0) {
            const pb_draw_sort_entry *cur = &entries[(uint32_t)j];
            bool move = false;

            if (back_to_front) {
                move = cur->view_depth < key.view_depth ||
                    (cur->view_depth == key.view_depth && cur->draw_index > key.draw_index);
            } else {
                move = cur->view_depth > key.view_depth ||
                    (cur->view_depth == key.view_depth && cur->draw_index > key.draw_index);
            }

            if (!move) {
                break;
            }

            entries[(uint32_t)j + 1] = entries[(uint32_t)j];
            --j;
        }

        entries[(uint32_t)j + 1] = key;
    }
}
