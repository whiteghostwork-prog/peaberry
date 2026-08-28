/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "peaberry/peaberry_math.h"
#include "test.h"

#include <math.h>

static float mat4_element(const pb_mat4 m, int row, int col)
{
    return m[col][row];
}

PB_TEST(test_radians)
{
    PB_ASSERT_FLOAT_EQ(pb_radians(0.0f), 0.0f, 1e-6f);
    PB_ASSERT_FLOAT_EQ(pb_radians(180.0f), 3.14159265359f, 1e-5f);
    PB_ASSERT_FLOAT_EQ(pb_radians(90.0f), 1.57079632679f, 1e-5f);
    PB_TEST_PASS();
}

PB_TEST(test_mat4_identity)
{
    pb_mat4 m = {0};
    pb_mat4_identity(m);

    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            const float expected = row == col ? 1.0f : 0.0f;
            PB_ASSERT_FLOAT_EQ(mat4_element(m, row, col), expected, 1e-6f);
        }
    }

    PB_TEST_PASS();
}

PB_TEST(test_mat4_mul_identity)
{
    pb_mat4 a = {0};
    pb_mat4 b = {0};
    pb_mat4 out = {0};

    pb_mat4_identity(a);
    pb_mat4_identity(b);
    pb_mat4_mul(a, b, out);

    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            const float expected = row == col ? 1.0f : 0.0f;
            PB_ASSERT_FLOAT_EQ(mat4_element(out, row, col), expected, 1e-5f);
        }
    }

    PB_TEST_PASS();
}

PB_TEST(test_mat4_perspective)
{
    pb_mat4 proj = {0};
    pb_mat4_identity(proj);
    pb_mat4_perspective(proj, pb_radians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f);

    PB_ASSERT(isfinite(mat4_element(proj, 0, 0)));
    PB_ASSERT(isfinite(mat4_element(proj, 1, 1)));
    PB_ASSERT(mat4_element(proj, 0, 0) > 0.0f);
    /* Vulkan NDC Y points down the screen, so pb_mat4_perspective negates
     * the Y scale to keep world-up at the top of the framebuffer. */
    PB_ASSERT(mat4_element(proj, 1, 1) < 0.0f);
    PB_ASSERT(mat4_element(proj, 2, 3) != 0.0f);
    PB_ASSERT(mat4_element(proj, 3, 2) != 0.0f);
    PB_TEST_PASS();
}

PB_TEST(test_mat4_look_at)
{
    pb_mat4 view = {0};
    pb_mat4_identity(view);

    const pb_vec3 eye = { 0.0f, 0.0f, 3.0f };
    const pb_vec3 center = { 0.0f, 0.0f, 0.0f };
    const pb_vec3 up = { 0.0f, 1.0f, 0.0f };
    pb_mat4_look_at(view, eye, center, up);

    PB_ASSERT(isfinite(mat4_element(view, 0, 0)));
    PB_ASSERT(isfinite(mat4_element(view, 1, 1)));
    PB_ASSERT(isfinite(mat4_element(view, 2, 2)));
    PB_ASSERT(isfinite(mat4_element(view, 3, 3)));
    PB_TEST_PASS();
}

PB_TEST(test_mat4_rotate_y)
{
    pb_mat4 m = {0};
    pb_mat4 out = {0};
    pb_mat4_identity(m);
    pb_mat4_rotate_y(m, pb_radians(90.0f), out);

    PB_ASSERT_FLOAT_EQ(mat4_element(out, 0, 0), 0.0f, 1e-4f);
    PB_ASSERT_FLOAT_EQ(mat4_element(out, 0, 2), 1.0f, 1e-4f);
    PB_ASSERT_FLOAT_EQ(mat4_element(out, 2, 0), -1.0f, 1e-4f);
    PB_ASSERT_FLOAT_EQ(mat4_element(out, 2, 2), 0.0f, 1e-4f);
    PB_TEST_PASS();
}

void pb_run_math_tests(void)
{
    printf("math tests\n");
    PB_RUN_TEST(test_radians);
    PB_RUN_TEST(test_mat4_identity);
    PB_RUN_TEST(test_mat4_mul_identity);
    PB_RUN_TEST(test_mat4_perspective);
    PB_RUN_TEST(test_mat4_look_at);
    PB_RUN_TEST(test_mat4_rotate_y);
}
