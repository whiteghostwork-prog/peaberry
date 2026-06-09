/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bench_target.h"

#include "pb_context_internal.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/alloc.h"
#include "rhi/texture.h"

#include <stdlib.h>
#include <string.h>

struct pb_bench_target {
    pb_context *context;
    pb_bench_scenario *scenario;
    pb_rhi_query_pool *query_pool;
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    pb_rhi_texture color;
    VkImage depth_image;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;
    VkFormat depth_format;
    VkExtent2D extent;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkFence fence;
    bool detailed;
};

static bool choose_depth_format(VkPhysicalDevice physical_device, VkFormat *out_format)
{
    const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physical_device, candidates[i], &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            *out_format = candidates[i];
            return true;
        }
    }

    return false;
}

static bool create_depth_image(pb_bench_target *target)
{
    VkDevice device = pb_context_device(target->context);
    VkPhysicalDevice physical_device = pb_context_physical_device(target->context);

    if (!choose_depth_format(physical_device, &target->depth_format)) {
        return false;
    }

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = target->depth_format,
        .extent = { target->extent.width, target->extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (vkCreateImage(device, &image_info, NULL, &target->depth_image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, target->depth_image, &mem_reqs);

    const pb_vk_context *vk = &target->context->vk;
    const uint32_t mem_type = pb_rhi_find_memory_type(
        vk,
        mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX) {
        return false;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type,
    };

    if (vkAllocateMemory(device, &alloc_info, NULL, &target->depth_memory) != VK_SUCCESS) {
        return false;
    }

    if (vkBindImageMemory(device, target->depth_image, target->depth_memory, 0) != VK_SUCCESS) {
        return false;
    }

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = target->depth_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = target->depth_format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };

    return vkCreateImageView(device, &view_info, NULL, &target->depth_view) == VK_SUCCESS;
}

static bool create_render_pass(pb_bench_target *target)
{
    VkAttachmentDescription attachments[2] = {
        {
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        },
        {
            .format = target->depth_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },
    };

    VkAttachmentReference color_ref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = {
        .attachment = 1,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
        .pDepthStencilAttachment = &depth_ref,
    };

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };

    return vkCreateRenderPass(pb_context_device(target->context), &render_pass_info, NULL, &target->render_pass) ==
        VK_SUCCESS;
}

static bool create_framebuffer(pb_bench_target *target)
{
    VkImageView attachments[] = { target->color.view, target->depth_view };
    VkFramebufferCreateInfo fb_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = target->render_pass,
        .attachmentCount = 2,
        .pAttachments = attachments,
        .width = target->extent.width,
        .height = target->extent.height,
        .layers = 1,
    };

    return vkCreateFramebuffer(pb_context_device(target->context), &fb_info, NULL, &target->framebuffer) ==
        VK_SUCCESS;
}

static bool create_command_resources(pb_bench_target *target)
{
    VkDevice device = pb_context_device(target->context);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = pb_context_graphics_queue_family(target->context),
    };

    if (vkCreateCommandPool(device, &pool_info, NULL, &target->command_pool) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = target->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    if (vkAllocateCommandBuffers(device, &alloc_info, &target->command_buffer) != VK_SUCCESS) {
        return false;
    }

    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    return vkCreateFence(device, &fence_info, NULL, &target->fence) == VK_SUCCESS;
}

bool pb_bench_target_create(
    pb_bench_target **out_target,
    pb_context *context,
    VkExtent2D extent,
    pb_bench_scenario *scenario,
    bool detailed)
{
    if (!out_target || !context || !scenario || extent.width == 0 || extent.height == 0) {
        return false;
    }

    pb_bench_target *target = calloc(1, sizeof(*target));
    if (!target) {
        return false;
    }

    target->context = context;
    target->scenario = scenario;
    target->extent = extent;
    target->detailed = detailed;

    if (!pb_rhi_query_pool_create(context, detailed, &target->query_pool) ||
        !create_depth_image(target) ||
        !pb_rhi_texture_create_2d(
            context,
            extent.width,
            extent.height,
            1,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            &target->color) ||
        !create_render_pass(target) ||
        !create_framebuffer(target) ||
        !create_command_resources(target) ||
        !scenario->setup(scenario, context, target->render_pass, extent)) {
        pb_bench_target_destroy(target);
        return false;
    }

    *out_target = target;
    return true;
}

void pb_bench_target_destroy(pb_bench_target *target)
{
    if (!target) {
        return;
    }

    if (target->scenario && target->scenario->teardown) {
        target->scenario->teardown(target->scenario);
    }

    if (target->context && pb_context_device_ready(target->context)) {
        pb_context_wait_device_idle(target->context);
        VkDevice device = pb_context_device(target->context);

        if (target->fence) {
            vkDestroyFence(device, target->fence, NULL);
        }
        if (target->command_pool) {
            vkDestroyCommandPool(device, target->command_pool, NULL);
        }
        if (target->framebuffer) {
            vkDestroyFramebuffer(device, target->framebuffer, NULL);
        }
        if (target->render_pass) {
            vkDestroyRenderPass(device, target->render_pass, NULL);
        }
        if (target->depth_view) {
            vkDestroyImageView(device, target->depth_view, NULL);
        }
        if (target->depth_image) {
            vkDestroyImage(device, target->depth_image, NULL);
        }
        if (target->depth_memory) {
            vkFreeMemory(device, target->depth_memory, NULL);
        }

        pb_rhi_texture_destroy(target->context, &target->color);
        pb_rhi_query_pool_destroy(target->context, target->query_pool);
    }

    free(target);
}

bool pb_bench_target_run_frame(pb_bench_target *target, pb_bench_frame *out_frame)
{
    if (!target || !out_frame) {
        return false;
    }

    pb_bench_frame_zero(out_frame);

    VkDevice device = pb_context_device(target->context);
    VkQueue queue = pb_context_graphics_queue(target->context);
    VkCommandBuffer cmd = target->command_buffer;

    vkResetFences(device, 1, &target->fence);
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
        return false;
    }

    pb_rhi_query_pool_cmd_reset(cmd, target->query_pool);
    pb_rhi_query_pool_write_timestamp(cmd, target->query_pool, PB_RHI_TS_CMD_START, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

    VkClearValue clears[2] = {
        { .color = { { 0.02f, 0.02f, 0.025f, 1.0f } } },
        { .depthStencil = { 1.0f, 0 } },
    };

    VkRenderPassBeginInfo rp_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = target->render_pass,
        .framebuffer = target->framebuffer,
        .renderArea = { .extent = target->extent },
        .clearValueCount = 2,
        .pClearValues = clears,
    };

    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    pb_rhi_query_pool_write_timestamp(
        cmd,
        target->query_pool,
        PB_RHI_TS_RENDER_PASS_START,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    if (target->scenario->record) {
        target->scenario->record(cmd, target->extent, target->scenario->user_data);
    }

    if (target->detailed) {
        pb_rhi_query_pool_write_timestamp(
            cmd,
            target->query_pool,
            PB_RHI_TS_DETAILED_VERTEX,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
        pb_rhi_query_pool_write_timestamp(
            cmd,
            target->query_pool,
            PB_RHI_TS_DETAILED_FRAGMENT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        pb_rhi_query_pool_write_timestamp(
            cmd,
            target->query_pool,
            PB_RHI_TS_DETAILED_RENDER_PASS_END,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        vkCmdEndRenderPass(cmd);
        pb_rhi_query_pool_write_timestamp(
            cmd,
            target->query_pool,
            PB_RHI_TS_DETAILED_TRANSFER,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
        pb_rhi_query_pool_write_timestamp(
            cmd,
            target->query_pool,
            PB_RHI_TS_DETAILED_CMD_END,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    } else {
        pb_rhi_query_pool_write_timestamp(
            cmd,
            target->query_pool,
            PB_RHI_TS_RENDER_PASS_END,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        vkCmdEndRenderPass(cmd);

        pb_rhi_query_pool_write_timestamp(
            cmd,
            target->query_pool,
            PB_RHI_TS_CMD_END,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        return false;
    }

    const uint64_t cpu_start = pb_bench_now_ns();

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };

    if (vkQueueSubmit(queue, 1, &submit_info, target->fence) != VK_SUCCESS) {
        return false;
    }

    if (vkWaitForFences(device, 1, &target->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        return false;
    }

    out_frame->cpu_submit_to_idle_ns = pb_bench_now_ns() - cpu_start;

    const uint32_t query_count = pb_rhi_query_pool_query_count(target->query_pool);
    uint64_t *ticks = calloc(query_count, sizeof(*ticks));
    if (!ticks) {
        return false;
    }

    if (!pb_rhi_query_pool_read_timestamps(target->context, target->query_pool, ticks, query_count)) {
        free(ticks);
        return false;
    }

    const bool ok = pb_rhi_query_pool_fill_frame(
        target->context,
        target->query_pool,
        ticks,
        query_count,
        out_frame);
    free(ticks);
    return ok;
}

VkRenderPass pb_bench_target_render_pass(const pb_bench_target *target)
{
    return target ? target->render_pass : VK_NULL_HANDLE;
}

VkExtent2D pb_bench_target_extent(const pb_bench_target *target)
{
    VkExtent2D extent = {0, 0};
    if (target) {
        extent = target->extent;
    }
    return extent;
}
