/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pbr/shadow_pass.h"
#include "test.h"

#include <math.h>

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
    PB_ASSERT(light_proj[2][2] > 0.0f);

    PB_TEST_PASS();
}

void pb_run_shadow_tests(void)
{
    printf("shadow tests\n");
    PB_RUN_TEST(test_shadow_light_matrices_fit_aabb);
}
