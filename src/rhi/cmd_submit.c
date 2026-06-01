/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rhi/cmd_submit.h"

#include "peaberry/peaberry_vk.h"

bool pb_rhi_submit_one_shot(
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
