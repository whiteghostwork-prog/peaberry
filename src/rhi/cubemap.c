/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rhi/cubemap.h"

#include "core/log.h"
#include "pb_context_internal.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/alloc.h"

static void cubemap_reset(pb_rhi_cubemap *cubemap)
{
    *cubemap = (pb_rhi_cubemap){0};
}

bool pb_rhi_cubemap_create(
    pb_context *context,
    uint32_t size,
    uint32_t mip_levels,
    VkFormat format,
    VkImageUsageFlags usage,
    pb_rhi_cubemap *cubemap)
{
    if (!context || !cubemap || size == 0 || mip_levels == 0) {
        return false;
    }

    const pb_vk_context *vk = &context->vk;
    VkDevice device = vk->device;

    cubemap_reset(cubemap);
    cubemap->format = format;
    cubemap->size = size;
    cubemap->mip_levels = mip_levels;

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { size, size, 1 },
        .mipLevels = mip_levels,
        .arrayLayers = 6,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (vkCreateImage(device, &image_info, NULL, &cubemap->image) != VK_SUCCESS) {
        pb_log_error("vkCreateImage failed for cubemap");
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, cubemap->image, &mem_reqs);

    uint32_t mem_type = pb_rhi_find_memory_type(
        vk,
        mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX) {
        pb_log_error("No device-local memory type for cubemap");
        vkDestroyImage(device, cubemap->image, NULL);
        cubemap_reset(cubemap);
        return false;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };

    if (vkAllocateMemory(device, &alloc_info, NULL, &cubemap->memory) != VK_SUCCESS) {
        vkDestroyImage(device, cubemap->image, NULL);
        cubemap_reset(cubemap);
        return false;
    }

    if (vkBindImageMemory(device, cubemap->image, cubemap->memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, cubemap->memory, NULL);
        vkDestroyImage(device, cubemap->image, NULL);
        cubemap_reset(cubemap);
        return false;
    }

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = cubemap->image,
        .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
        .format = format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = mip_levels,
            .layerCount = 6,
        },
    };

    if (vkCreateImageView(device, &view_info, NULL, &cubemap->view) != VK_SUCCESS) {
        pb_log_error("vkCreateImageView failed for cubemap");
        pb_rhi_cubemap_destroy(context, cubemap);
        return false;
    }

    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .maxLod = (float)mip_levels,
    };

    if (vkCreateSampler(device, &sampler_info, NULL, &cubemap->sampler) != VK_SUCCESS) {
        pb_log_error("vkCreateSampler failed for cubemap");
        pb_rhi_cubemap_destroy(context, cubemap);
        return false;
    }

    return true;
}

bool pb_rhi_cubemap_create_face_view(
    pb_context *context,
    const pb_rhi_cubemap *cubemap,
    uint32_t face,
    uint32_t mip_level,
    VkImageView *out_view)
{
    if (!context || !cubemap || !out_view || face >= 6 || mip_level >= cubemap->mip_levels) {
        return false;
    }

    VkDevice device = pb_context_device(context);

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = cubemap->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = cubemap->format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = mip_level,
            .levelCount = 1,
            .baseArrayLayer = face,
            .layerCount = 1,
        },
    };

    return vkCreateImageView(device, &view_info, NULL, out_view) == VK_SUCCESS;
}

void pb_rhi_cubemap_destroy(pb_context *context, pb_rhi_cubemap *cubemap)
{
    if (!context || !cubemap || cubemap->image == VK_NULL_HANDLE) {
        return;
    }

    VkDevice device = pb_context_device(context);

    if (cubemap->sampler) {
        vkDestroySampler(device, cubemap->sampler, NULL);
    }
    if (cubemap->view) {
        vkDestroyImageView(device, cubemap->view, NULL);
    }
    if (cubemap->image) {
        vkDestroyImage(device, cubemap->image, NULL);
    }
    if (cubemap->memory) {
        vkFreeMemory(device, cubemap->memory, NULL);
    }

    cubemap_reset(cubemap);
}
