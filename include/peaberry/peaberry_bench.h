/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPU/CPU frame timing for benchmarks. Unused struct slots remain zero.
 */

#ifndef PEABERRY_BENCH_H
#define PEABERRY_BENCH_H

#include "peaberry/peaberry.h"

#include <stdbool.h>
#include <stdint.h>
#include <volk.h>

typedef struct pb_bench_frame {
    /* GPU timestamps (nanoseconds, 0 if unavailable) */
    uint64_t gpu_total_ns;
    uint64_t gpu_render_pass_ns;
    uint64_t gpu_present_ns;
    uint64_t gpu_vertex_ns;
    uint64_t gpu_fragment_ns;
    uint64_t gpu_transfer_ns;

    /* CPU timing (nanoseconds) */
    uint64_t cpu_submit_to_idle_ns;
    uint64_t cpu_present_ns;
} pb_bench_frame;

typedef struct pb_rhi_query_pool pb_rhi_query_pool;

/* Monotonic wall clock in nanoseconds (CLOCK_MONOTONIC). */
uint64_t pb_bench_now_ns(void);

void pb_bench_frame_zero(pb_bench_frame *frame);

bool pb_bench_gpu_timestamps_available(pb_context *context);

bool pb_rhi_query_pool_create(pb_context *context, bool detailed, pb_rhi_query_pool **out_pool);
void pb_rhi_query_pool_destroy(pb_context *context, pb_rhi_query_pool *pool);

uint32_t pb_rhi_query_pool_query_count(const pb_rhi_query_pool *pool);
bool pb_rhi_query_pool_is_detailed(const pb_rhi_query_pool *pool);

void pb_rhi_query_pool_cmd_reset(VkCommandBuffer cmd, const pb_rhi_query_pool *pool);

void pb_rhi_query_pool_write_timestamp(
    VkCommandBuffer cmd,
    const pb_rhi_query_pool *pool,
    uint32_t query_index,
    VkPipelineStageFlagBits stage);

/*
 * Basic four-query layout:
 *   0 cmd start (TOP_OF_PIPE)
 *   1 render pass start (COLOR_ATTACHMENT_OUTPUT)
 *   2 render pass end   (COLOR_ATTACHMENT_OUTPUT)
 *   3 cmd end           (BOTTOM_OF_PIPE)
 *
 * Detailed seven-query layout (--detailed):
 *   0 cmd start
 *   1 render pass start
 *   2 vertex shader complete (VERTEX_SHADER_BIT, after draws)
 *   3 fragment shader complete (FRAGMENT_SHADER_BIT)
 *   4 render pass end (COLOR_ATTACHMENT_OUTPUT)
 *   5 transfer complete (TRANSFER_BIT, after render pass)
 *   6 cmd end (BOTTOM_OF_PIPE)
 */
enum {
    PB_RHI_TS_CMD_START = 0,
    PB_RHI_TS_RENDER_PASS_START = 1,
    PB_RHI_TS_RENDER_PASS_END = 2,
    PB_RHI_TS_CMD_END = 3,
    PB_RHI_TS_QUERY_COUNT = 4,

    PB_RHI_TS_DETAILED_VERTEX = 2,
    PB_RHI_TS_DETAILED_FRAGMENT = 3,
    PB_RHI_TS_DETAILED_RENDER_PASS_END = 4,
    PB_RHI_TS_DETAILED_TRANSFER = 5,
    PB_RHI_TS_DETAILED_CMD_END = 6,
    PB_RHI_TS_QUERY_COUNT_DETAILED = 7,
};

bool pb_rhi_query_pool_read_timestamps(
    pb_context *context,
    const pb_rhi_query_pool *pool,
    uint64_t *out_ticks,
    uint32_t count);

bool pb_rhi_query_pool_fill_frame(
    pb_context *context,
    const pb_rhi_query_pool *pool,
    const uint64_t *ticks,
    uint32_t tick_count,
    pb_bench_frame *out);

#endif
