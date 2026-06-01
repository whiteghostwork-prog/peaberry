/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_RHI_TEXTURE_H
#define PEABERRY_RHI_TEXTURE_H

#include "peaberry/peaberry.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <volk.h>

typedef struct pb_rhi_texture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSampler sampler;
    VkFormat format;
    uint32_t width;
    uint32_t height;
    uint32_t mip_levels;
} pb_rhi_texture;

bool pb_rhi_texture_create_2d(
    pb_context *context,
    uint32_t width,
    uint32_t height,
    uint32_t mip_levels,
    VkFormat format,
    VkImageUsageFlags usage,
    pb_rhi_texture *texture);

bool pb_rhi_texture_create_from_file(
    pb_context *context,
    const char *path,
    bool srgb,
    pb_rhi_texture *texture);

bool pb_rhi_texture_create_from_memory(
    pb_context *context,
    const void *data,
    size_t data_size,
    bool srgb,
    pb_rhi_texture *texture);

bool pb_rhi_texture_create_solid_rgba8(
    pb_context *context,
    const uint8_t rgba[4],
    bool srgb,
    pb_rhi_texture *texture);

bool pb_rhi_texture_create_from_hdr_file(
    pb_context *context,
    const char *path,
    pb_rhi_texture *texture);

bool pb_rhi_texture_upload_rgba32f(
    pb_context *context,
    pb_rhi_texture *texture,
    const float *pixels,
    uint32_t width,
    uint32_t height);

void pb_rhi_texture_transition_layout(
    VkCommandBuffer cmd,
    pb_rhi_texture *texture,
    VkImageLayout old_layout,
    VkImageLayout new_layout,
    uint32_t mip_levels,
    uint32_t layer_count);

void pb_rhi_texture_destroy(pb_context *context, pb_rhi_texture *texture);

#endif
