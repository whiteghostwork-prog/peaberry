/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_bench.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/cmd_submit.h"
#include "test.h"

#include <string.h>

typedef struct bench_record_ctx {
    pb_rhi_query_pool *pool;
} bench_record_ctx;

static void record_timestamp_smoke(VkCommandBuffer cmd, void *user_data)
{
    bench_record_ctx *ctx = user_data;
    pb_rhi_query_pool_cmd_reset(cmd, ctx->pool);
    pb_rhi_query_pool_write_timestamp(cmd, ctx->pool, PB_RHI_TS_CMD_START, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    pb_rhi_query_pool_write_timestamp(
        cmd, ctx->pool, PB_RHI_TS_RENDER_PASS_START, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    pb_rhi_query_pool_write_timestamp(
        cmd, ctx->pool, PB_RHI_TS_RENDER_PASS_END, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    pb_rhi_query_pool_write_timestamp(cmd, ctx->pool, PB_RHI_TS_CMD_END, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

PB_TEST(test_bench_now_ns_monotonic)
{
    const uint64_t a = pb_bench_now_ns();
    const uint64_t b = pb_bench_now_ns();
    PB_ASSERT(a > 0);
    PB_ASSERT(b >= a);
    PB_TEST_PASS();
}

PB_TEST(test_bench_query_pool_smoke)
{
    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry bench test",
            .enable_validation = false,
            .enable_surface = false,
        });
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    pb_rhi_query_pool *pool = NULL;
    PB_ASSERT(pb_rhi_query_pool_create(ctx, false, &pool));
    PB_ASSERT(pool != NULL);

    if (!pb_bench_gpu_timestamps_available(ctx)) {
        pb_rhi_query_pool_destroy(ctx, pool);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("GPU timestamps not supported");
    }

    bench_record_ctx record_ctx = { .pool = pool };
    PB_ASSERT(pb_rhi_submit_one_shot(ctx, record_timestamp_smoke, &record_ctx));

    uint64_t ticks[PB_RHI_TS_QUERY_COUNT] = {0};
    PB_ASSERT(pb_rhi_query_pool_read_timestamps(ctx, pool, ticks, PB_RHI_TS_QUERY_COUNT));

    pb_bench_frame frame;
    PB_ASSERT(pb_rhi_query_pool_fill_frame(ctx, pool, ticks, PB_RHI_TS_QUERY_COUNT, &frame));
    PB_ASSERT(frame.gpu_total_ns > 0);

    pb_rhi_query_pool_destroy(ctx, pool);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

static void record_detailed_timestamp_smoke(VkCommandBuffer cmd, void *user_data)
{
    bench_record_ctx *ctx = user_data;
    pb_rhi_query_pool_cmd_reset(cmd, ctx->pool);
    pb_rhi_query_pool_write_timestamp(cmd, ctx->pool, PB_RHI_TS_CMD_START, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    pb_rhi_query_pool_write_timestamp(
        cmd, ctx->pool, PB_RHI_TS_RENDER_PASS_START, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    pb_rhi_query_pool_write_timestamp(
        cmd, ctx->pool, PB_RHI_TS_DETAILED_VERTEX, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
    pb_rhi_query_pool_write_timestamp(
        cmd, ctx->pool, PB_RHI_TS_DETAILED_FRAGMENT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    pb_rhi_query_pool_write_timestamp(
        cmd, ctx->pool, PB_RHI_TS_DETAILED_RENDER_PASS_END, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    pb_rhi_query_pool_write_timestamp(
        cmd, ctx->pool, PB_RHI_TS_DETAILED_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT);
    pb_rhi_query_pool_write_timestamp(
        cmd, ctx->pool, PB_RHI_TS_DETAILED_CMD_END, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

PB_TEST(test_bench_query_pool_detailed)
{
    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry bench detailed test",
            .enable_validation = false,
            .enable_surface = false,
        });
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    pb_rhi_query_pool *pool = NULL;
    PB_ASSERT(pb_rhi_query_pool_create(ctx, true, &pool));
    PB_ASSERT(pool != NULL);
    PB_ASSERT(pb_rhi_query_pool_is_detailed(pool));
    PB_ASSERT(pb_rhi_query_pool_query_count(pool) == PB_RHI_TS_QUERY_COUNT_DETAILED);

    if (!pb_bench_gpu_timestamps_available(ctx)) {
        pb_rhi_query_pool_destroy(ctx, pool);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("GPU timestamps not supported");
    }

    bench_record_ctx record_ctx = { .pool = pool };
    PB_ASSERT(pb_rhi_submit_one_shot(ctx, record_detailed_timestamp_smoke, &record_ctx));

    uint64_t ticks[PB_RHI_TS_QUERY_COUNT_DETAILED] = {0};
    PB_ASSERT(pb_rhi_query_pool_read_timestamps(ctx, pool, ticks, PB_RHI_TS_QUERY_COUNT_DETAILED));

    pb_bench_frame frame;
    PB_ASSERT(
        pb_rhi_query_pool_fill_frame(ctx, pool, ticks, PB_RHI_TS_QUERY_COUNT_DETAILED, &frame));
    PB_ASSERT(frame.gpu_total_ns > 0);
    PB_ASSERT(frame.gpu_render_pass_ns > 0);
    PB_ASSERT(frame.gpu_vertex_ns > 0);
    PB_ASSERT(frame.gpu_fragment_ns > 0);

    pb_rhi_query_pool_destroy(ctx, pool);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

void pb_run_bench_tests(void)
{
    printf("bench tests\n");
    PB_RUN_TEST(test_bench_now_ns_monotonic);
    PB_RUN_TEST(test_bench_query_pool_smoke);
    PB_RUN_TEST(test_bench_query_pool_detailed);
}
