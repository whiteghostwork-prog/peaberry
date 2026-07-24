/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rhi/texture.h"

#include "core/log.h"
#include "pb_context_internal.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/alloc.h"
#include "rhi/buffer.h"
#include "rhi/cmd_submit.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <limits.h>

static void texture_reset(pb_rhi_texture *texture)
{
    *texture = (pb_rhi_texture){0};
}

static bool create_sampler(
    pb_context *context,
    pb_rhi_texture *texture,
    VkSamplerAddressMode address_mode,
    float max_lod)
{
    VkDevice device = pb_context_device(context);

    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = address_mode,
        .addressModeV = address_mode,
        .addressModeW = address_mode,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .maxLod = max_lod,
    };

    return vkCreateSampler(device, &sampler_info, NULL, &texture->sampler) == VK_SUCCESS;
}

static bool create_image_internal(
    pb_context *context,
    uint32_t width,
    uint32_t height,
    uint32_t mip_levels,
    VkFormat format,
    VkImageUsageFlags usage,
    pb_rhi_texture *texture)
{
    const pb_vk_context *vk = &context->vk;
    VkDevice device = vk->device;

    texture->format = format;
    texture->width = width;
    texture->height = height;
    texture->mip_levels = mip_levels;

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = mip_levels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (vkCreateImage(device, &image_info, NULL, &texture->image) != VK_SUCCESS) {
        pb_log_error("vkCreateImage failed");
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, texture->image, &mem_reqs);

    uint32_t mem_type = pb_rhi_find_memory_type(
        vk,
        mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX) {
        pb_log_error("No device-local memory type for texture");
        vkDestroyImage(device, texture->image, NULL);
        texture_reset(texture);
        return false;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };

    if (vkAllocateMemory(device, &alloc_info, NULL, &texture->memory) != VK_SUCCESS) {
        vkDestroyImage(device, texture->image, NULL);
        texture_reset(texture);
        return false;
    }

    if (vkBindImageMemory(device, texture->image, texture->memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, texture->memory, NULL);
        vkDestroyImage(device, texture->image, NULL);
        texture_reset(texture);
        return false;
    }

    return true;
}

static bool create_view(pb_context *context, pb_rhi_texture *texture)
{
    VkDevice device = pb_context_device(context);

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = texture->format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = texture->mip_levels,
            .layerCount = 1,
        },
    };

    return vkCreateImageView(device, &view_info, NULL, &texture->view) == VK_SUCCESS;
}

void pb_rhi_texture_transition_layout(
    VkCommandBuffer cmd,
    pb_rhi_texture *texture,
    VkImageLayout old_layout,
    VkImageLayout new_layout,
    uint32_t mip_levels,
    uint32_t layer_count)
{
    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags src_access = 0;
    VkAccessFlags dst_access = 0;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        src_access = 0;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        src_access = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        src_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        src_access = VK_ACCESS_SHADER_READ_BIT;
    }

    if (new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_access = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else if (new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dst_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    } else if (new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        /* Phase 15.2: compute shaders (auto-exposure histogram) also sample the
         * shader-read-only image, so the dst stage must cover both fragment and
         * compute. Adding COMPUTE_SHADER is harmless for fragment-only reads. */
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dst_access = VK_ACCESS_SHADER_READ_BIT;
    }

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .image = texture->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = mip_levels,
            .layerCount = layer_count,
        },
    };

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

typedef struct upload_context {
    pb_rhi_texture *texture;
    pb_rhi_buffer *staging;
} upload_context;

static void record_texture_upload(VkCommandBuffer cmd, void *user_data)
{
    upload_context *ctx = user_data;
    pb_rhi_texture *texture = ctx->texture;

    pb_rhi_texture_transition_layout(
        cmd,
        texture,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        1);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageExtent = {
            .width = texture->width,
            .height = texture->height,
            .depth = 1,
        },
    };

    vkCmdCopyBufferToImage(
        cmd,
        pb_rhi_buffer_handle(ctx->staging),
        texture->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region);

    pb_rhi_texture_transition_layout(
        cmd,
        texture,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        1,
        1);
}

bool pb_rhi_texture_create_2d(
    pb_context *context,
    uint32_t width,
    uint32_t height,
    uint32_t mip_levels,
    VkFormat format,
    VkImageUsageFlags usage,
    pb_rhi_texture *texture)
{
    if (!context || !texture || width == 0 || height == 0 || mip_levels == 0) {
        return false;
    }

    texture_reset(texture);

    if (!create_image_internal(context, width, height, mip_levels, format, usage, texture)) {
        return false;
    }

    if (!create_view(context, texture)) {
        pb_rhi_texture_destroy(context, texture);
        return false;
    }

    if (!create_sampler(context, texture, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, (float)mip_levels)) {
        pb_rhi_texture_destroy(context, texture);
        return false;
    }

    return true;
}

bool pb_rhi_texture_upload_rgba32f(
    pb_context *context,
    pb_rhi_texture *texture,
    const float *pixels,
    uint32_t width,
    uint32_t height)
{
    if (!context || !texture || !pixels || width != texture->width || height != texture->height) {
        return false;
    }

    const VkDeviceSize image_size = (VkDeviceSize)width * (VkDeviceSize)height * 4 * sizeof(float);

    pb_rhi_buffer staging = {0};
    pb_rhi_buffer_desc staging_desc = {
        .size = image_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };

    if (!pb_rhi_buffer_create(context, &staging_desc, &staging)) {
        return false;
    }

    if (!pb_rhi_buffer_upload(context, &staging, pixels, image_size)) {
        pb_rhi_buffer_destroy(context, &staging);
        return false;
    }

    upload_context upload = {
        .texture = texture,
        .staging = &staging,
    };

    if (!pb_rhi_submit_one_shot(context, record_texture_upload, &upload)) {
        pb_rhi_buffer_destroy(context, &staging);
        return false;
    }

    pb_rhi_buffer_destroy(context, &staging);
    return true;
}

static bool create_rgba8_texture_from_pixels(
    pb_context *context,
    const stbi_uc *pixels,
    int width,
    int height,
    bool srgb,
    pb_rhi_texture *texture)
{
    if (!pixels || width <= 0 || height <= 0) {
        return false;
    }

    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    const VkDeviceSize image_size = (VkDeviceSize)width * (VkDeviceSize)height * 4;

    if (!create_image_internal(
            context,
            (uint32_t)width,
            (uint32_t)height,
            1,
            format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            texture)) {
        return false;
    }

    pb_rhi_buffer staging = {0};
    pb_rhi_buffer_desc staging_desc = {
        .size = image_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };

    if (!pb_rhi_buffer_create(context, &staging_desc, &staging)) {
        pb_rhi_texture_destroy(context, texture);
        return false;
    }

    if (!pb_rhi_buffer_upload(context, &staging, pixels, image_size)) {
        pb_rhi_buffer_destroy(context, &staging);
        pb_rhi_texture_destroy(context, texture);
        return false;
    }

    upload_context upload = {
        .texture = texture,
        .staging = &staging,
    };

    if (!pb_rhi_submit_one_shot(context, record_texture_upload, &upload)) {
        pb_rhi_buffer_destroy(context, &staging);
        pb_rhi_texture_destroy(context, texture);
        return false;
    }

    pb_rhi_buffer_destroy(context, &staging);

    if (!create_view(context, texture)) {
        pb_rhi_texture_destroy(context, texture);
        return false;
    }

    if (!create_sampler(context, texture, VK_SAMPLER_ADDRESS_MODE_REPEAT, 1.0f)) {
        pb_rhi_texture_destroy(context, texture);
        return false;
    }

    return true;
}

bool pb_rhi_texture_create_from_file(
    pb_context *context,
    const char *path,
    bool srgb,
    pb_rhi_texture *texture)
{
    if (!context || !path || !texture) {
        return false;
    }

    if (!pb_context_device_ready(context)) {
        pb_log_error("Vulkan device is not initialized");
        return false;
    }

    texture_reset(texture);

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load(path, &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels) {
        pb_log_error("Failed to load texture: %s", path);
        return false;
    }

    const bool ok = create_rgba8_texture_from_pixels(context, pixels, width, height, srgb, texture);
    stbi_image_free(pixels);

    if (ok) {
        pb_log_info("Loaded texture %s (%ux%u)", path, texture->width, texture->height);
    }
    return ok;
}

bool pb_rhi_texture_create_from_memory(
    pb_context *context,
    const void *data,
    size_t data_size,
    bool srgb,
    pb_rhi_texture *texture)
{
    if (!context || !data || data_size == 0 || !texture) {
        return false;
    }

    if (!pb_context_device_ready(context)) {
        pb_log_error("Vulkan device is not initialized");
        return false;
    }

    if (data_size > (size_t)INT_MAX) {
        pb_log_error("Texture buffer too large for stb_image (%zu bytes)", data_size);
        return false;
    }

    texture_reset(texture);

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load_from_memory(data, (int)data_size, &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels) {
        pb_log_error("Failed to decode texture from memory");
        return false;
    }

    const bool ok = create_rgba8_texture_from_pixels(context, pixels, width, height, srgb, texture);
    stbi_image_free(pixels);
    return ok;
}

bool pb_rhi_texture_create_solid_rgba8(
    pb_context *context,
    const uint8_t rgba[4],
    bool srgb,
    pb_rhi_texture *texture)
{
    if (!context || !rgba || !texture) {
        return false;
    }

    if (!pb_context_device_ready(context)) {
        return false;
    }

    texture_reset(texture);
    return create_rgba8_texture_from_pixels(context, rgba, 1, 1, srgb, texture);
}

bool pb_rhi_texture_create_from_hdr_file(
    pb_context *context,
    const char *path,
    pb_rhi_texture *texture)
{
    if (!context || !path || !texture) {
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    float *pixels = stbi_loadf(path, &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0) {
        pb_log_error("Failed to load HDR texture: %s", path);
        stbi_image_free(pixels);
        return false;
    }

    texture_reset(texture);

    if (!pb_rhi_texture_create_2d(
            context,
            (uint32_t)width,
            (uint32_t)height,
            1,
            VK_FORMAT_R32G32B32A32_SFLOAT,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            texture)) {
        stbi_image_free(pixels);
        return false;
    }

    const bool ok = pb_rhi_texture_upload_rgba32f(
        context,
        texture,
        pixels,
        (uint32_t)width,
        (uint32_t)height);

    stbi_image_free(pixels);

    if (!ok) {
        pb_rhi_texture_destroy(context, texture);
        return false;
    }

    pb_log_info("Loaded HDR texture %s (%ux%u)", path, texture->width, texture->height);
    return true;
}

void pb_rhi_texture_destroy(pb_context *context, pb_rhi_texture *texture)
{
    if (!context || !texture || texture->image == VK_NULL_HANDLE) {
        return;
    }

    VkDevice device = pb_context_device(context);

    if (texture->sampler) {
        vkDestroySampler(device, texture->sampler, NULL);
    }
    if (texture->view) {
        vkDestroyImageView(device, texture->view, NULL);
    }
    if (texture->image) {
        vkDestroyImage(device, texture->image, NULL);
    }
    if (texture->memory) {
        vkFreeMemory(device, texture->memory, NULL);
    }

    texture_reset(texture);
}
