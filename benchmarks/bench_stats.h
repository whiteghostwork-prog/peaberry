/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_BENCH_STATS_H
#define PEABERRY_BENCH_STATS_H

#include <stdint.h>

typedef struct pb_bench_stats {
    uint64_t min;
    uint64_t p50;
    uint64_t p95;
    uint64_t p99;
    uint64_t max;
    uint64_t mean;
    double stddev;
    uint32_t count;
} pb_bench_stats;

void pb_bench_stats_compute(const uint64_t *samples, uint32_t count, pb_bench_stats *out);

uint64_t pb_bench_stats_percentile(const uint64_t *sorted, uint32_t count, uint32_t percentile);

#endif
