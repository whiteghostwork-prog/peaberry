#ifndef PEABERRY_RHI_CUBEMAP_H
#define PEABERRY_RHI_CUBEMAP_H

#include "peaberry/peaberry.h"

#include <stdbool.h>
#include <stdint.h>
#include <volk.h>

typedef struct pb_rhi_cubemap {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSampler sampler;
    VkFormat format;
    uint32_t size;
    uint32_t mip_levels;
} pb_rhi_cubemap;

bool pb_rhi_cubemap_create(
    pb_context *context,
    uint32_t size,
    uint32_t mip_levels,
    VkFormat format,
    VkImageUsageFlags usage,
    pb_rhi_cubemap *cubemap);

bool pb_rhi_cubemap_create_face_view(
    pb_context *context,
    const pb_rhi_cubemap *cubemap,
    uint32_t face,
    uint32_t mip_level,
    VkImageView *out_view);

void pb_rhi_cubemap_destroy(pb_context *context, pb_rhi_cubemap *cubemap);

#endif
