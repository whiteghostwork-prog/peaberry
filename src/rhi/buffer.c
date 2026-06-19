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

#include "rhi/buffer.h"

#include "core/log.h"
#include "peaberry/peaberry_vk.h"
#include "pb_context_internal.h"

#include <string.h>

static void buffer_reset(pb_rhi_buffer *buffer)
{
    *buffer = (pb_rhi_buffer){0};
}

bool pb_rhi_buffer_create(
    pb_context *context,
    const pb_rhi_buffer_desc *desc,
    pb_rhi_buffer *buffer)
{
    if (!context || !desc || !buffer || desc->size == 0) {
        return false;
    }

    if (!pb_context_device_ready(context)) {
        pb_log_error("Vulkan device is not initialized");
        return false;
    }

    const pb_vk_context *vk = &context->vk;
    VkDevice device = vk->device;

    buffer_reset(buffer);

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = desc->size,
        .usage = desc->usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    if (vkCreateBuffer(device, &buffer_info, NULL, &buffer->handle) != VK_SUCCESS) {
        pb_log_error("vkCreateBuffer failed");
        buffer_reset(buffer);
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(device, buffer->handle, &mem_reqs);

    VkMemoryPropertyFlags mem_props = pb_rhi_memory_properties(desc->memory_usage);
    uint32_t mem_type = pb_rhi_find_memory_type(vk, mem_reqs.memoryTypeBits, mem_props);
    if (mem_type == UINT32_MAX) {
        pb_log_error("No suitable memory type for buffer");
        vkDestroyBuffer(device, buffer->handle, NULL);
        buffer_reset(buffer);
        return false;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };

    if (vkAllocateMemory(device, &alloc_info, NULL, &buffer->memory) != VK_SUCCESS) {
        pb_log_error("vkAllocateMemory failed");
        vkDestroyBuffer(device, buffer->handle, NULL);
        buffer_reset(buffer);
        return false;
    }

    if (vkBindBufferMemory(device, buffer->handle, buffer->memory, 0) != VK_SUCCESS) {
        pb_log_error("vkBindBufferMemory failed");
        vkFreeMemory(device, buffer->memory, NULL);
        vkDestroyBuffer(device, buffer->handle, NULL);
        buffer_reset(buffer);
        return false;
    }

    buffer->size = desc->size;
    buffer->host_visible =
        (mem_props & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (buffer->host_visible) {
        void *mapped = NULL;
        if (vkMapMemory(device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            pb_log_error("vkMapMemory failed for host-visible buffer");
            vkFreeMemory(device, buffer->memory, NULL);
            vkDestroyBuffer(device, buffer->handle, NULL);
            buffer_reset(buffer);
            return false;
        }

        buffer->mapped = mapped;
    }

    return true;
}

void pb_rhi_buffer_destroy(pb_context *context, pb_rhi_buffer *buffer)
{
    if (!context || !buffer || buffer->handle == VK_NULL_HANDLE) {
        return;
    }

    VkDevice device = context->vk.device;
    if (buffer->mapped) {
        vkUnmapMemory(device, buffer->memory);
    }
    vkDestroyBuffer(device, buffer->handle, NULL);
    if (buffer->memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, buffer->memory, NULL);
    }
    buffer_reset(buffer);
}

VkBuffer pb_rhi_buffer_handle(const pb_rhi_buffer *buffer)
{
    return buffer ? buffer->handle : VK_NULL_HANDLE;
}

void *pb_rhi_buffer_mapped(const pb_rhi_buffer *buffer)
{
    return buffer ? buffer->mapped : NULL;
}

bool pb_rhi_buffer_write(
    pb_rhi_buffer *buffer,
    VkDeviceSize offset,
    const void *data,
    VkDeviceSize size)
{
    if (!buffer || !data || size == 0 || offset + size > buffer->size) {
        return false;
    }

    if (!buffer->host_visible || !buffer->mapped) {
        pb_log_error("Buffer is not persistently mapped");
        return false;
    }

    memcpy((char *)buffer->mapped + offset, data, (size_t)size);
    return true;
}

bool pb_rhi_buffer_upload(
    pb_context *context,
    pb_rhi_buffer *buffer,
    const void *data,
    VkDeviceSize size)
{
    if (!context || !buffer || !data || size == 0 || size > buffer->size) {
        return false;
    }

    if (!buffer->host_visible) {
        pb_log_error("Buffer is not host-visible; staging upload not implemented");
        return false;
    }

    if (buffer->mapped) {
        return pb_rhi_buffer_write(buffer, 0, data, size);
    }

    void *mapped = NULL;
    if (vkMapMemory(context->vk.device, buffer->memory, 0, size, 0, &mapped) != VK_SUCCESS) {
        pb_log_error("vkMapMemory failed");
        return false;
    }

    memcpy(mapped, data, (size_t)size);
    vkUnmapMemory(context->vk.device, buffer->memory);
    return true;
}
