/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/alloc.h"
#include "rhi/buffer.h"
#include "rhi/ring_buffer.h"
#include "test.h"

#include <string.h>

PB_TEST(test_rhi_buffer_persistent_map)
{
    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry ring buffer test",
            .enable_validation = false,
            .enable_surface = false,
        });
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    pb_rhi_buffer buffer = {0};
    pb_rhi_buffer_desc desc = {
        .size = 64,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };
    PB_ASSERT(pb_rhi_buffer_create(ctx, &desc, &buffer));
    PB_ASSERT(buffer.mapped != NULL);

    const uint32_t payload = 0xAABBCCDDu;
    PB_ASSERT(pb_rhi_buffer_write(&buffer, 0, &payload, sizeof(payload)));
    PB_ASSERT(memcmp(buffer.mapped, &payload, sizeof(payload)) == 0);

    pb_rhi_buffer_destroy(ctx, &buffer);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

PB_TEST(test_rhi_ring_buffer_slots)
{
    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry ring buffer test",
            .enable_validation = false,
            .enable_surface = false,
        });
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    pb_rhi_ring_buffer ring = {0};
    const VkDeviceSize slot_size = 32;
    const VkDeviceSize alignment = pb_rhi_min_uniform_buffer_offset_alignment(ctx);
    PB_ASSERT(
        pb_rhi_ring_buffer_create(
            ctx,
            slot_size,
            PB_RHI_FRAMES_IN_FLIGHT,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            alignment,
            &ring));

    PB_ASSERT(ring.slot_count == PB_RHI_FRAMES_IN_FLIGHT);
    PB_ASSERT(ring.slot_stride >= slot_size);
    PB_ASSERT(pb_rhi_ring_buffer_slot_offset(&ring, 1) == ring.slot_stride);

    const uint8_t slot0[32] = {1};
    const uint8_t slot1[32] = {2};
    PB_ASSERT(pb_rhi_ring_buffer_write_slot(&ring, 0, slot0, sizeof(slot0)));
    PB_ASSERT(pb_rhi_ring_buffer_write_slot(&ring, 1, slot1, sizeof(slot1)));

    void *mapped0 = pb_rhi_ring_buffer_slot_host(&ring, 0);
    void *mapped1 = pb_rhi_ring_buffer_slot_host(&ring, 1);
    PB_ASSERT(mapped0 != NULL && mapped1 != NULL);
    PB_ASSERT(((uint8_t *)mapped0)[0] == 1);
    PB_ASSERT(((uint8_t *)mapped1)[0] == 2);

    pb_rhi_ring_buffer_destroy(ctx, &ring);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

void pb_run_ring_buffer_tests(void)
{
    printf("ring buffer tests\n");
    PB_RUN_TEST(test_rhi_buffer_persistent_map);
    PB_RUN_TEST(test_rhi_ring_buffer_slots);
}
