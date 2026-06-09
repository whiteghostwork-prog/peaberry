/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "rhi/query_pool.h"

#include "core/log.h"
#include "peaberry/peaberry_vk.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t pb_bench_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void pb_bench_frame_zero(pb_bench_frame *frame)
{
    if (frame) {
        memset(frame, 0, sizeof(*frame));
    }
}

bool pb_bench_gpu_timestamps_available(pb_context *context)
{
    if (!context || !pb_context_device_ready(context)) {
        return false;
    }

    VkPhysicalDevice physical_device = pb_context_physical_device(context);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device, &props);

    return props.limits.timestampComputeAndGraphics == VK_TRUE;
}

static uint64_t ticks_to_ns(const pb_rhi_query_pool *pool, uint64_t start, uint64_t end)
{
    if (!pool || !pool->timestamps_supported || end <= start) {
        return 0;
    }

    const double delta = (double)(end - start) * (double)pool->timestamp_period_ns;
    if (delta <= 0.0) {
        return 0;
    }

    return (uint64_t)delta;
}

bool pb_rhi_query_pool_create(pb_context *context, bool detailed, pb_rhi_query_pool **out_pool)
{
    if (!context || !out_pool || !pb_context_device_ready(context)) {
        return false;
    }

    pb_rhi_query_pool *pool = calloc(1, sizeof(*pool));
    if (!pool) {
        return false;
    }

    pool->detailed = detailed;
    pool->query_count = detailed ? PB_RHI_TS_QUERY_COUNT_DETAILED : PB_RHI_TS_QUERY_COUNT;

    VkPhysicalDevice physical_device = pb_context_physical_device(context);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device, &props);

    pool->timestamp_period_ns = props.limits.timestampPeriod;
    pool->timestamps_supported = props.limits.timestampComputeAndGraphics == VK_TRUE;

    if (!pool->timestamps_supported) {
        pb_log_info("GPU timestamp queries are not supported on this device");
        *out_pool = pool;
        return true;
    }

    VkQueryPoolCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = pool->query_count,
    };

    VkDevice device = pb_context_device(context);
    if (vkCreateQueryPool(device, &create_info, NULL, &pool->handle) != VK_SUCCESS) {
        pb_log_error("Failed to create timestamp query pool");
        free(pool);
        return false;
    }

    *out_pool = pool;
    return true;
}

uint32_t pb_rhi_query_pool_query_count(const pb_rhi_query_pool *pool)
{
    return pool ? pool->query_count : 0;
}

bool pb_rhi_query_pool_is_detailed(const pb_rhi_query_pool *pool)
{
    return pool && pool->detailed;
}

void pb_rhi_query_pool_destroy(pb_context *context, pb_rhi_query_pool *pool)
{
    if (!pool) {
        return;
    }

    if (context && pb_context_device_ready(context) && pool->handle) {
        vkDestroyQueryPool(pb_context_device(context), pool->handle, NULL);
    }

    free(pool);
}

void pb_rhi_query_pool_cmd_reset(VkCommandBuffer cmd, const pb_rhi_query_pool *pool)
{
    if (!cmd || !pool || !pool->timestamps_supported || !pool->handle) {
        return;
    }

    vkCmdResetQueryPool(cmd, pool->handle, 0, pool->query_count);
}

void pb_rhi_query_pool_write_timestamp(
    VkCommandBuffer cmd,
    const pb_rhi_query_pool *pool,
    uint32_t query_index,
    VkPipelineStageFlagBits stage)
{
    if (!cmd || !pool || !pool->timestamps_supported || !pool->handle) {
        return;
    }

    if (query_index >= pool->query_count) {
        return;
    }

    vkCmdWriteTimestamp(cmd, stage, pool->handle, query_index);
}

bool pb_rhi_query_pool_read_timestamps(
    pb_context *context,
    const pb_rhi_query_pool *pool,
    uint64_t *out_ticks,
    uint32_t count)
{
    if (!context || !pool || !out_ticks || count == 0) {
        return false;
    }

    if (!pool->timestamps_supported || !pool->handle) {
        memset(out_ticks, 0, (size_t)count * sizeof(*out_ticks));
        return true;
    }

    if (count > pool->query_count) {
        count = pool->query_count;
    }

    VkDevice device = pb_context_device(context);
    const VkResult result = vkGetQueryPoolResults(
        device,
        pool->handle,
        0,
        count,
        (size_t)count * sizeof(*out_ticks),
        out_ticks,
        sizeof(*out_ticks),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    return result == VK_SUCCESS;
}

bool pb_rhi_query_pool_fill_frame(
    pb_context *context,
    const pb_rhi_query_pool *pool,
    const uint64_t *ticks,
    uint32_t tick_count,
    pb_bench_frame *out)
{
    if (!out) {
        return false;
    }

    pb_bench_frame_zero(out);

    const uint32_t required = pool ? pool->query_count : PB_RHI_TS_QUERY_COUNT;
    if (!pool || !ticks || tick_count < required) {
        return false;
    }

    if (pool->detailed) {
        out->gpu_total_ns =
            ticks_to_ns(pool, ticks[PB_RHI_TS_CMD_START], ticks[PB_RHI_TS_DETAILED_CMD_END]);
        out->gpu_render_pass_ns = ticks_to_ns(
            pool,
            ticks[PB_RHI_TS_RENDER_PASS_START],
            ticks[PB_RHI_TS_DETAILED_RENDER_PASS_END]);
        out->gpu_vertex_ns = ticks_to_ns(
            pool,
            ticks[PB_RHI_TS_RENDER_PASS_START],
            ticks[PB_RHI_TS_DETAILED_VERTEX]);
        out->gpu_fragment_ns = ticks_to_ns(
            pool,
            ticks[PB_RHI_TS_DETAILED_VERTEX],
            ticks[PB_RHI_TS_DETAILED_FRAGMENT]);
        out->gpu_transfer_ns = ticks_to_ns(
            pool,
            ticks[PB_RHI_TS_DETAILED_TRANSFER],
            ticks[PB_RHI_TS_DETAILED_CMD_END]);
    } else {
        out->gpu_total_ns = ticks_to_ns(pool, ticks[PB_RHI_TS_CMD_START], ticks[PB_RHI_TS_CMD_END]);
        out->gpu_render_pass_ns =
            ticks_to_ns(pool, ticks[PB_RHI_TS_RENDER_PASS_START], ticks[PB_RHI_TS_RENDER_PASS_END]);
    }

    (void)context;
    return true;
}
