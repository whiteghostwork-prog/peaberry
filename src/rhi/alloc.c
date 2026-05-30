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

#include "rhi/alloc.h"

bool pb_rhi_alloc_init(pb_vk_context *ctx)
{
    return ctx && ctx->device != VK_NULL_HANDLE && ctx->physical_device != VK_NULL_HANDLE;
}

void pb_rhi_alloc_shutdown(pb_vk_context *ctx)
{
    (void)ctx;
}

uint32_t pb_rhi_find_memory_type(
    const pb_vk_context *ctx,
    uint32_t type_filter,
    VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    return UINT32_MAX;
}

VkMemoryPropertyFlags pb_rhi_memory_properties(pb_rhi_memory_usage usage)
{
    switch (usage) {
    case PB_RHI_MEMORY_CPU_TO_GPU:
        return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    case PB_RHI_MEMORY_GPU_ONLY:
        return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    default:
        return 0;
    }
}
