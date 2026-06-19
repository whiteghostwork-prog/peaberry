/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rhi/ring_buffer.h"

#include "core/log.h"
#include "rhi/alloc.h"

#include <string.h>

bool pb_rhi_ring_buffer_create(
    pb_context *context,
    VkDeviceSize slot_size,
    uint32_t slot_count,
    VkBufferUsageFlags usage,
    VkDeviceSize alignment,
    pb_rhi_ring_buffer *out)
{
    if (!context || !out || slot_size == 0 || slot_count == 0) {
        return false;
    }

    *out = (pb_rhi_ring_buffer){0};

    const VkDeviceSize aligned_slot = pb_rhi_align_up(slot_size, alignment > 0 ? alignment : 1);
    const VkDeviceSize total_size = aligned_slot * slot_count;

    pb_rhi_buffer_desc desc = {
        .size = total_size,
        .usage = usage,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };

    if (!pb_rhi_buffer_create(context, &desc, &out->buffer)) {
        return false;
    }

    out->slot_stride = aligned_slot;
    out->slot_size = slot_size;
    out->slot_count = slot_count;
    return true;
}

void pb_rhi_ring_buffer_destroy(pb_context *context, pb_rhi_ring_buffer *ring)
{
    if (!ring) {
        return;
    }

    pb_rhi_buffer_destroy(context, &ring->buffer);
    *ring = (pb_rhi_ring_buffer){0};
}

VkBuffer pb_rhi_ring_buffer_handle(const pb_rhi_ring_buffer *ring)
{
    return ring ? pb_rhi_buffer_handle(&ring->buffer) : VK_NULL_HANDLE;
}

VkDeviceSize pb_rhi_ring_buffer_slot_offset(const pb_rhi_ring_buffer *ring, uint32_t slot)
{
    if (!ring || slot >= ring->slot_count) {
        return 0;
    }

    return ring->slot_stride * slot;
}

void *pb_rhi_ring_buffer_slot_host(pb_rhi_ring_buffer *ring, uint32_t slot)
{
    if (!ring || slot >= ring->slot_count) {
        return NULL;
    }

    void *base = pb_rhi_buffer_mapped(&ring->buffer);
    if (!base) {
        return NULL;
    }

    return (char *)base + pb_rhi_ring_buffer_slot_offset(ring, slot);
}

bool pb_rhi_ring_buffer_write_slot(
    pb_rhi_ring_buffer *ring,
    uint32_t slot,
    const void *data,
    VkDeviceSize size)
{
    if (!ring || !data || size == 0 || size > ring->slot_size || slot >= ring->slot_count) {
        return false;
    }

    return pb_rhi_buffer_write(
        &ring->buffer,
        pb_rhi_ring_buffer_slot_offset(ring, slot),
        data,
        size);
}
