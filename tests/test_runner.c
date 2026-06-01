/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test.h"

pb_test_stats g_pb_test_stats;

void pb_test_reset_stats(void)
{
    g_pb_test_stats = (pb_test_stats){0};
}

void pb_test_report(const char *suite)
{
    printf(
        "%s: %d passed, %d failed, %d skipped\n",
        suite,
        g_pb_test_stats.passed,
        g_pb_test_stats.failed,
        g_pb_test_stats.skipped);
}
