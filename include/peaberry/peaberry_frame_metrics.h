/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Per-frame FPS and CPU/GPU load derived from peaberry_bench timing fields.
 */

#ifndef PEABERRY_FRAME_METRICS_H
#define PEABERRY_FRAME_METRICS_H

#include "peaberry/peaberry_bench.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    PB_FRAME_BUDGET_60HZ_NS = 16666666u,
};

typedef struct pb_frame_metrics {
    uint64_t wall_frame_ns;
    uint64_t gpu_total_ns;
    uint64_t gpu_render_pass_ns;
    uint64_t cpu_submit_to_idle_ns;
    uint64_t cpu_present_ns;
    double wall_fps;
    double gpu_fps;
    double gpu_load_percent;
} pb_frame_metrics;

typedef struct pb_frame_metrics_accumulator {
    double window_seconds;
    double window_start_s;
    uint32_t frames_in_window;
    double wall_fps;
} pb_frame_metrics_accumulator;

void pb_frame_metrics_zero(pb_frame_metrics *metrics);

void pb_frame_metrics_from_bench_frame(
    const pb_bench_frame *bench,
    uint64_t wall_frame_ns,
    uint64_t cpu_present_ns,
    uint64_t frame_budget_ns,
    pb_frame_metrics *out);

double pb_frame_metrics_fps_from_ns(uint64_t frame_ns);

void pb_frame_metrics_accumulator_init(pb_frame_metrics_accumulator *acc, double window_seconds);

void pb_frame_metrics_accumulator_reset(pb_frame_metrics_accumulator *acc, double now_s);

bool pb_frame_metrics_accumulator_push(
    pb_frame_metrics_accumulator *acc,
    double now_s,
    uint64_t wall_frame_ns);

int pb_frame_metrics_format_overlay(
    const pb_frame_metrics *metrics,
    char *out,
    size_t out_size);

#endif
