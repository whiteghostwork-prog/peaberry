/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pbr/shadow_pass.h"
#include "test.h"

#include "peaberry/peaberry_math.h"

#include <math.h>

static void transform_point(const pb_mat4 m, const float in[3], float out[4])
{
    out[0] = m[0][0] * in[0] + m[1][0] * in[1] + m[2][0] * in[2] + m[3][0];
    out[1] = m[0][1] * in[0] + m[1][1] * in[1] + m[2][1] * in[2] + m[3][1];
    out[2] = m[0][2] * in[0] + m[1][2] * in[1] + m[2][2] * in[2] + m[3][2];
    out[3] = m[0][3] * in[0] + m[1][3] * in[1] + m[2][3] * in[2] + m[3][3];
}

PB_TEST(test_shadow_light_matrices_fit_aabb)
{
    const float light_dir[3] = { 0.5f, 0.8f, 0.4f };
    const float bounds_min[3] = { -1.0f, -1.0f, -1.0f };
    const float bounds_max[3] = { 1.0f, 1.0f, 1.0f };
    pb_mat4 light_view = {0};
    pb_mat4 light_proj = {0};

    PB_ASSERT(pb_shadow_light_matrices_fit_aabb(
        light_dir,
        bounds_min,
        bounds_max,
        light_view,
        light_proj));

    PB_ASSERT(fabsf(light_view[3][3] - 1.0f) < 1e-5f);
    PB_ASSERT(fabsf(light_proj[3][3] - 1.0f) < 1e-5f);
    PB_ASSERT(light_proj[0][0] > 0.0f);
    PB_ASSERT(light_proj[1][1] > 0.0f);
    /* Right-handed ortho with 0..1 depth scales Z negatively. */
    PB_ASSERT(light_proj[2][2] < 0.0f);

    PB_TEST_PASS();
}

PB_TEST(test_shadow_light_frustum_contains_bounds)
{
    const float light_dir[3] = { 0.5f, 0.8f, 0.4f };
    const float bounds_min[3] = { -1.0f, -1.0f, -1.0f };
    const float bounds_max[3] = { 1.0f, 1.0f, 1.0f };
    pb_mat4 light_view = {0};
    pb_mat4 light_proj = {0};
    pb_mat4 light_vp = {0};

    PB_ASSERT(pb_shadow_light_matrices_fit_aabb(
        light_dir,
        bounds_min,
        bounds_max,
        light_view,
        light_proj));
    pb_mat4_mul(light_proj, light_view, light_vp);

    for (uint32_t corner = 0; corner < 8; ++corner) {
        const float world[3] = {
            (corner & 1u) ? bounds_max[0] : bounds_min[0],
            (corner & 2u) ? bounds_max[1] : bounds_min[1],
            (corner & 4u) ? bounds_max[2] : bounds_min[2],
        };
        float view[4];
        transform_point(light_view, world, view);
        float clip[4];
        transform_point(light_vp, world, clip);
        PB_ASSERT(clip[3] != 0.0f);

        const float inv_w = 1.0f / clip[3];
        const float ndc_x = clip[0] * inv_w;
        const float ndc_y = clip[1] * inv_w;
        const float ndc_z = clip[2] * inv_w;
        const float uv_x = ndc_x * 0.5f + 0.5f;
        const float uv_y = ndc_y * 0.5f + 0.5f;

        PB_ASSERT(uv_x >= 0.0f && uv_x <= 1.0f);
        PB_ASSERT(uv_y >= 0.0f && uv_y <= 1.0f);
        PB_ASSERT(ndc_z >= 0.0f && ndc_z <= 1.0f);
    }

    PB_TEST_PASS();
}

void pb_run_shadow_tests(void)
{
    printf("shadow tests\n");
    PB_RUN_TEST(test_shadow_light_matrices_fit_aabb);
    PB_RUN_TEST(test_shadow_light_frustum_contains_bounds);
}
