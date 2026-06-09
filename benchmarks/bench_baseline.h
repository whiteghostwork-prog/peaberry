/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_BENCH_BASELINE_H
#define PEABERRY_BENCH_BASELINE_H

#include <stdbool.h>
#include <stdint.h>

enum {
    PB_BENCH_BASELINE_VERSION = 1,
    PB_BENCH_BASELINE_SCENARIO_MAX = 64,
    PB_BENCH_BASELINE_SCENARIO_ARG_MAX = 512,
    PB_BENCH_BASELINE_GPU_MAX = 256,
    PB_BENCH_BASELINE_MODE_MAX = 32,
};

typedef struct pb_bench_baseline {
    uint32_t version;
    char scenario[PB_BENCH_BASELINE_SCENARIO_MAX];
    char scenario_arg[PB_BENCH_BASELINE_SCENARIO_ARG_MAX];
    uint32_t width;
    uint32_t height;
    char gpu[PB_BENCH_BASELINE_GPU_MAX];
    char mode[PB_BENCH_BASELINE_MODE_MAX];
    uint64_t gpu_render_pass_p95;
} pb_bench_baseline;

typedef struct pb_bench_compare_run {
    const char *scenario_name;
    const char *scenario_arg;
    uint32_t width;
    uint32_t height;
    const char *gpu_name;
    uint64_t gpu_render_pass_p95;
} pb_bench_compare_run;

bool pb_bench_baseline_load(const char *path, pb_bench_baseline *out);

bool pb_bench_baseline_compare(
    const pb_bench_baseline *baseline,
    const pb_bench_compare_run *run,
    float tolerance_percent,
    double *out_delta_percent);

#endif
