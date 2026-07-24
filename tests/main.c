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
void pb_run_draw_sort_tests(void);
void pb_run_shadow_tests(void);
void pb_run_frustum_cull_tests(void);
void pb_run_ring_buffer_tests(void);

int main(void)
{
    /* Line-buffer stdout so test results are flushed as they're printed.
     * Lavapipe's worker threads intermittently crash during process exit
     * (a known driver bug); line-buffering ensures the results reach disk
     * even if the crash interrupts the final teardown. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    pb_test_reset_stats();

    pb_run_math_tests();
    pb_run_vulkan_tests();
    pb_run_gltf_tests();
    pb_run_gltf_render_tests();
    pb_run_draw_sort_tests();
    pb_run_shadow_tests();
    pb_run_frustum_cull_tests();
    pb_run_ring_buffer_tests();
    pb_run_bench_tests();

    pb_test_report("total");
    fflush(stdout);

    if (g_pb_test_stats.failed > 0) {
        return EXIT_FAILURE;
    }

    if (g_pb_test_stats.passed == 0 && g_pb_test_stats.skipped > 0) {
        fprintf(stderr, "all tests skipped\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
