/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pbr/draw_sort.h"
#include "test.h"

#include <string.h>

PB_TEST(test_draw_sort_view_depth)
{
    pb_mat4 view;
    pb_mat4_identity(view);

    pb_mat4 near_model;
    pb_mat4_identity(near_model);
    near_model[3][2] = 1.0f;

    pb_mat4 far_model;
    pb_mat4_identity(far_model);
    far_model[3][2] = 4.0f;

    PB_ASSERT(pb_draw_sort_view_depth(view, near_model) < pb_draw_sort_view_depth(view, far_model));
    PB_TEST_PASS();
}

PB_TEST(test_draw_sort_opaque_front_to_back)
{
    pb_draw_sort_entry entries[] = {
        { .draw_index = 2, .view_depth = 0.8f },
        { .draw_index = 0, .view_depth = 0.2f },
        { .draw_index = 1, .view_depth = 0.2f },
    };

    pb_draw_sort_stable(entries, 3, false);

    PB_ASSERT(entries[0].draw_index == 0);
    PB_ASSERT(entries[1].draw_index == 1);
    PB_ASSERT(entries[2].draw_index == 2);
    PB_TEST_PASS();
}

PB_TEST(test_draw_sort_blend_back_to_front)
{
    pb_draw_sort_entry entries[] = {
        { .draw_index = 1, .view_depth = 0.2f },
        { .draw_index = 0, .view_depth = 0.8f },
        { .draw_index = 2, .view_depth = 0.8f },
    };

    pb_draw_sort_stable(entries, 3, true);

    PB_ASSERT(entries[0].draw_index == 0);
    PB_ASSERT(entries[1].draw_index == 2);
    PB_ASSERT(entries[2].draw_index == 1);
    PB_TEST_PASS();
}

PB_TEST(test_draw_sort_bounds_far_point)
{
    pb_mat4 view;
    pb_mat4_identity(view);

    pb_mat4 model;
    pb_mat4_identity(model);
    model[3][2] = 2.0f;

    const float bounds_min[3] = { -0.5f, -0.5f, -0.5f };
    const float bounds_max[3] = { 0.5f, 0.5f, 0.5f };

    const float center_depth = pb_draw_sort_view_depth(view, model);
    const float far_depth = pb_draw_sort_view_depth_bounds(
        view, model, bounds_min, bounds_max, true);
    const float near_depth = pb_draw_sort_view_depth_bounds(
        view, model, bounds_min, bounds_max, false);

    PB_ASSERT(far_depth > center_depth);
    PB_ASSERT(near_depth < center_depth);
    PB_TEST_PASS();
}

PB_TEST(test_draw_sort_blend_distance_order)
{
    const float camera[3] = { 0.0f, 0.0f, 0.0f };
    const float bounds_min[3] = { -0.5f, -0.5f, -0.5f };
    const float bounds_max[3] = { 0.5f, 0.5f, 0.5f };

    pb_mat4 near_model;
    pb_mat4_identity(near_model);
    near_model[3][2] = 1.0f;

    pb_mat4 far_model;
    pb_mat4_identity(far_model);
    far_model[3][2] = 3.0f;

    const float near_dist = pb_draw_sort_blend_distance(camera, near_model, bounds_min, bounds_max);
    const float far_dist = pb_draw_sort_blend_distance(camera, far_model, bounds_min, bounds_max);

    PB_ASSERT(far_dist > near_dist);
    PB_TEST_PASS();
}

void pb_run_draw_sort_tests(void)
{
    printf("draw sort tests\n");
    PB_RUN_TEST(test_draw_sort_view_depth);
    PB_RUN_TEST(test_draw_sort_opaque_front_to_back);
    PB_RUN_TEST(test_draw_sort_blend_back_to_front);
    PB_RUN_TEST(test_draw_sort_bounds_far_point);
    PB_RUN_TEST(test_draw_sort_blend_distance_order);
}
