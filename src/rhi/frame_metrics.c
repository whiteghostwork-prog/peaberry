/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "peaberry/peaberry_frame_metrics.h"

#include <stdio.h>
#include <string.h>

void pb_frame_metrics_zero(pb_frame_metrics *metrics)
{
    if (metrics) {
        memset(metrics, 0, sizeof(*metrics));
    }
}

double pb_frame_metrics_fps_from_ns(uint64_t frame_ns)
{
    if (frame_ns == 0) {
        return 0.0;
    }

    return 1e9 / (double)frame_ns;
}

void pb_frame_metrics_from_bench_frame(
    const pb_bench_frame *bench,
    uint64_t wall_frame_ns,
    uint64_t cpu_present_ns,
    uint64_t frame_budget_ns,
    pb_frame_metrics *out)
{
    if (!out) {
        return;
    }

    pb_frame_metrics_zero(out);

    if (!bench) {
        return;
    }

    out->wall_frame_ns = wall_frame_ns;
    out->gpu_total_ns = bench->gpu_total_ns;
    out->gpu_render_pass_ns = bench->gpu_render_pass_ns;
    out->cpu_submit_to_idle_ns = bench->cpu_submit_to_idle_ns;
    out->cpu_present_ns = cpu_present_ns;

    if (wall_frame_ns > 0) {
        out->wall_fps = pb_frame_metrics_fps_from_ns(wall_frame_ns);
    }

    if (bench->gpu_total_ns > 0) {
        out->gpu_fps = pb_frame_metrics_fps_from_ns(bench->gpu_total_ns);
    }

    if (frame_budget_ns > 0 && bench->gpu_total_ns > 0) {
        out->gpu_load_percent = (double)bench->gpu_total_ns / (double)frame_budget_ns * 100.0;
    }
}

void pb_frame_metrics_accumulator_init(pb_frame_metrics_accumulator *acc, double window_seconds)
{
    if (!acc) {
        return;
    }

    memset(acc, 0, sizeof(*acc));
    acc->window_seconds = window_seconds > 0.0 ? window_seconds : 1.0;
}

void pb_frame_metrics_accumulator_reset(pb_frame_metrics_accumulator *acc, double now_s)
{
    if (!acc) {
        return;
    }

    acc->window_start_s = now_s;
    acc->frames_in_window = 0;
    acc->wall_fps = 0.0;
}

bool pb_frame_metrics_accumulator_push(
    pb_frame_metrics_accumulator *acc,
    double now_s,
    uint64_t wall_frame_ns)
{
    if (!acc || wall_frame_ns == 0) {
        return false;
    }

    if (acc->frames_in_window == 0) {
        acc->window_start_s = now_s;
    }

    acc->frames_in_window++;

    const double elapsed = now_s - acc->window_start_s;
    if (elapsed < acc->window_seconds) {
        return false;
    }

    if (elapsed > 0.0) {
        acc->wall_fps = (double)acc->frames_in_window / elapsed;
    }

    acc->window_start_s = now_s;
    acc->frames_in_window = 0;
    return true;
}

int pb_frame_metrics_format_overlay(
    const pb_frame_metrics *metrics,
    char *out,
    size_t out_size)
{
    if (!metrics || !out || out_size == 0) {
        return 0;
    }

    const double gpu_ms = metrics->gpu_total_ns / 1e6;
    const double cpu_ms = metrics->cpu_submit_to_idle_ns / 1e6;
    const double present_ms = metrics->cpu_present_ns / 1e6;

    return snprintf(
        out,
        out_size,
        "FPS %.1f | GPU %.2f ms (%.0f%%) | CPU %.2f ms | present %.2f ms",
        metrics->wall_fps,
        gpu_ms,
        metrics->gpu_load_percent,
        cpu_ms,
        present_ms);
}
