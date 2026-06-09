/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bench_stats.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int compare_u64(const void *a, const void *b)
{
    const uint64_t lhs = *(const uint64_t *)a;
    const uint64_t rhs = *(const uint64_t *)b;
    if (lhs < rhs) {
        return -1;
    }
    if (lhs > rhs) {
        return 1;
    }
    return 0;
}

uint64_t pb_bench_stats_percentile(const uint64_t *sorted, uint32_t count, uint32_t percentile)
{
    if (!sorted || count == 0) {
        return 0;
    }

    if (percentile >= 100) {
        return sorted[count - 1];
    }

    const uint32_t index = (count * percentile) / 100;
    const uint32_t clamped = index >= count ? count - 1 : index;
    return sorted[clamped];
}

void pb_bench_stats_compute(const uint64_t *samples, uint32_t count, pb_bench_stats *out)
{
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->count = count;

    if (!samples || count == 0) {
        return;
    }

    uint64_t *sorted = malloc((size_t)count * sizeof(*sorted));
    if (!sorted) {
        return;
    }

    memcpy(sorted, samples, (size_t)count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), compare_u64);

    out->min = sorted[0];
    out->max = sorted[count - 1];
    out->p50 = pb_bench_stats_percentile(sorted, count, 50);
    out->p95 = pb_bench_stats_percentile(sorted, count, 95);
    out->p99 = pb_bench_stats_percentile(sorted, count, 99);

    uint64_t sum = 0;
    for (uint32_t i = 0; i < count; ++i) {
        sum += samples[i];
    }

    out->mean = sum / count;

    double variance = 0.0;
    for (uint32_t i = 0; i < count; ++i) {
        const double delta = (double)samples[i] - (double)out->mean;
        variance += delta * delta;
    }

    out->stddev = sqrt(variance / (double)count);
    free(sorted);
}
