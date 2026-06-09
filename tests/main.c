/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test.h"

#include <stdio.h>
#include <stdlib.h>

void pb_run_math_tests(void);
void pb_run_vulkan_tests(void);
void pb_run_gltf_tests(void);
void pb_run_gltf_render_tests(void);
void pb_run_bench_tests(void);

int main(void)
{
    pb_test_reset_stats();

    pb_run_math_tests();
    pb_run_vulkan_tests();
    pb_run_gltf_tests();
    pb_run_gltf_render_tests();
    pb_run_bench_tests();

    pb_test_report("total");

    if (g_pb_test_stats.failed > 0) {
        return EXIT_FAILURE;
    }

    if (g_pb_test_stats.passed == 0 && g_pb_test_stats.skipped > 0) {
        fprintf(stderr, "all tests skipped\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
