/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pbr/frustum_cull.h"
#include "peaberry/peaberry_math.h"
#include "test.h"

PB_TEST(test_frustum_cull_center_visible)
{
    pb_mat4 view = {0};
    pb_mat4 proj = {0};
    pb_mat4_identity(view);
    pb_mat4_identity(proj);

    const pb_vec3 eye = { 0.0f, 0.0f, 3.0f };
    const pb_vec3 center = { 0.0f, 0.0f, 0.0f };
    const pb_vec3 up = { 0.0f, 1.0f, 0.0f };
    pb_mat4_look_at(view, eye, center, up);
    pb_mat4_perspective(proj, pb_radians(45.0f), 1.0f, 0.1f, 100.0f);

    pb_frustum frustum = {0};
    pb_frustum_from_view_proj(view, proj, &frustum);

    pb_mat4 world = {0};
    pb_mat4_identity(world);
    const float bounds_min[3] = { -0.5f, -0.5f, -0.5f };
    const float bounds_max[3] = { 0.5f, 0.5f, 0.5f };

    PB_ASSERT(pb_frustum_intersects_bounds(&frustum, world, bounds_min, bounds_max));
    PB_TEST_PASS();
}

PB_TEST(test_frustum_cull_far_outside)
{
    pb_mat4 view = {0};
    pb_mat4 proj = {0};

    const pb_vec3 eye = { 0.0f, 0.0f, 3.0f };
    const pb_vec3 center = { 0.0f, 0.0f, 0.0f };
    const pb_vec3 up = { 0.0f, 1.0f, 0.0f };
    pb_mat4_look_at(view, eye, center, up);
    pb_mat4_perspective(proj, pb_radians(45.0f), 1.0f, 0.1f, 100.0f);

    pb_frustum frustum = {0};
    pb_frustum_from_view_proj(view, proj, &frustum);

    pb_mat4 world = {0};
    pb_mat4_identity(world);
    world[3][0] = 20.0f;

    const float bounds_min[3] = { -0.5f, -0.5f, -0.5f };
    const float bounds_max[3] = { 0.5f, 0.5f, 0.5f };

    PB_ASSERT(!pb_frustum_intersects_bounds(&frustum, world, bounds_min, bounds_max));
    PB_TEST_PASS();
}

void pb_run_frustum_cull_tests(void)
{
    printf("frustum cull tests\n");
    PB_RUN_TEST(test_frustum_cull_center_visible);
    PB_RUN_TEST(test_frustum_cull_far_outside);
}
