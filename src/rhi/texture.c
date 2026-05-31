/*
 * Copyright 2026 The Peaberry Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "rhi/texture.h"

#include "core/log.h"
#include "pb_context_internal.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/alloc.h"
#include "rhi/buffer.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static void texture_reset(pb_rhi_texture *texture)
{
    *texture = (pb_rhi_texture){0};
}

static bool submit_one_shot(
    pb_context *context,
    void (*record)(VkCommandBuffer cmd, void *user_data),
    void *user_data)
{
    VkDevice device = pb_context_device(context);
    VkQueue queue = pb_context_graphics_queue(context);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = pb_context_graphics_queue_family(context),
    };

    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device, &pool_info, NULL, &pool) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &alloc_info, &cmd) != VK_SUCCESS) {
        vkDestroyCommandPool(device, pool, NULL);
        return false;
    }

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };

    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(device, &fence_info, NULL, &fence) != VK_SUCCESS) {
        vkDestroyCommandPool(device, pool, NULL);
        return false;
    }

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    bool ok = vkBeginCommandBuffer(cmd, &begin_info) == VK_SUCCESS;
    if (ok) {
        record(cmd, user_data);
        ok = vkEndCommandBuffer(cmd) == VK_SUCCESS;
    }

    if (ok) {
        VkSubmitInfo submit_info = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd,
        };
        ok = vkQueueSubmit(queue, 1, &submit_info, fence) == VK_SUCCESS;
    }

    if (ok) {
        ok = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    }

    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    return ok;
}

typedef struct upload_context {
    pb_rhi_texture *texture;
    pb_rhi_buffer *staging;
    VkDeviceSize image_size;
} upload_context;

static void record_texture_upload(VkCommandBuffer cmd, void *user_data)
{
    upload_context *ctx = user_data;
    pb_rhi_texture *texture = ctx->texture;

    VkImageMemoryBarrier to_transfer = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = texture->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        NULL,
        0,
        NULL,
        1,
        &to_transfer);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
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

    VkImageMemoryBarrier to_shader = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .image = texture->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0,
        NULL,
        0,
        NULL,
        1,
        &to_shader);
}

static bool create_image(
    pb_context *context,
    uint32_t width,
    uint32_t height,
    VkFormat format,
    pb_rhi_texture *texture)
{
    const pb_vk_context *vk = &context->vk;
    VkDevice device = vk->device;

    texture->format = format;

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
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
        pb_log_error("vkAllocateMemory failed for texture");
        vkDestroyImage(device, texture->image, NULL);
        texture_reset(texture);
        return false;
    }

    if (vkBindImageMemory(device, texture->image, texture->memory, 0) != VK_SUCCESS) {
        pb_log_error("vkBindImageMemory failed");
        vkFreeMemory(device, texture->memory, NULL);
        vkDestroyImage(device, texture->image, NULL);
        texture_reset(texture);
        return false;
    }

    texture->width = width;
    texture->height = height;
    return true;
}

static bool create_view_and_sampler(pb_context *context, pb_rhi_texture *texture)
{
    VkDevice device = pb_context_device(context);

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = texture->format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };

    if (vkCreateImageView(device, &view_info, NULL, &texture->view) != VK_SUCCESS) {
        pb_log_error("vkCreateImageView failed");
        return false;
    }

    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
    };

    if (vkCreateSampler(device, &sampler_info, NULL, &texture->sampler) != VK_SUCCESS) {
        pb_log_error("vkCreateSampler failed");
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
    if (!pixels || width <= 0 || height <= 0) {
        pb_log_error("Failed to load texture: %s", path);
        stbi_image_free(pixels);
        return false;
    }

    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    const VkDeviceSize image_size = (VkDeviceSize)width * (VkDeviceSize)height * 4;

    if (!create_image(context, (uint32_t)width, (uint32_t)height, format, texture)) {
        stbi_image_free(pixels);
        return false;
    }

    pb_rhi_buffer staging = {0};
    pb_rhi_buffer_desc staging_desc = {
        .size = image_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };

    if (!pb_rhi_buffer_create(context, &staging_desc, &staging)) {
        pb_log_error("Failed to create staging buffer for texture upload");
        stbi_image_free(pixels);
        pb_rhi_texture_destroy(context, texture);
        return false;
    }

    if (!pb_rhi_buffer_upload(context, &staging, pixels, image_size)) {
        pb_log_error("Failed to upload staging buffer for texture");
        stbi_image_free(pixels);
        pb_rhi_buffer_destroy(context, &staging);
        pb_rhi_texture_destroy(context, texture);
        return false;
    }

    stbi_image_free(pixels);

    upload_context upload = {
        .texture = texture,
        .staging = &staging,
        .image_size = image_size,
    };

    if (!submit_one_shot(context, record_texture_upload, &upload)) {
        pb_log_error("Texture upload submit failed");
        pb_rhi_buffer_destroy(context, &staging);
        pb_rhi_texture_destroy(context, texture);
        return false;
    }

    pb_rhi_buffer_destroy(context, &staging);

    if (!create_view_and_sampler(context, texture)) {
        pb_rhi_texture_destroy(context, texture);
        return false;
    }

    pb_log_info("Loaded texture %s (%ux%u)", path, texture->width, texture->height);
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
