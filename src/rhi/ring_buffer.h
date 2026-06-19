/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_RHI_RING_BUFFER_H
#define PEABERRY_RHI_RING_BUFFER_H

#include "rhi/buffer.h"

#include <stdbool.h>
#include <stdint.h>
#include <volk.h>

typedef struct pb_rhi_ring_buffer {
    pb_rhi_buffer buffer;
    VkDeviceSize slot_stride;
    VkDeviceSize slot_size;
    uint32_t slot_count;
} pb_rhi_ring_buffer;

bool pb_rhi_ring_buffer_create(
    pb_context *context,
    VkDeviceSize slot_size,
    uint32_t slot_count,
    VkBufferUsageFlags usage,
    VkDeviceSize alignment,
    pb_rhi_ring_buffer *out);

void pb_rhi_ring_buffer_destroy(pb_context *context, pb_rhi_ring_buffer *ring);

VkBuffer pb_rhi_ring_buffer_handle(const pb_rhi_ring_buffer *ring);

VkDeviceSize pb_rhi_ring_buffer_slot_offset(const pb_rhi_ring_buffer *ring, uint32_t slot);

void *pb_rhi_ring_buffer_slot_host(pb_rhi_ring_buffer *ring, uint32_t slot);

bool pb_rhi_ring_buffer_write_slot(
    pb_rhi_ring_buffer *ring,
    uint32_t slot,
    const void *data,
    VkDeviceSize size);

#endif
