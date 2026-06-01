/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rhi/offscreen.h"

#include "pb_context_internal.h"
#include "peaberry/peaberry_vk.h"

static void target_reset(pb_rhi_offscreen_target *target)
{
    *target = (pb_rhi_offscreen_target){0};
}

bool pb_rhi_offscreen_render_pass_create(
    pb_context *context,
    VkFormat color_format,
    VkRenderPass *out_render_pass)
{
    if (!context || !out_render_pass) {
        return false;
    }

    VkDevice device = pb_context_device(context);

    VkAttachmentDescription color_attachment = {
        .format = color_format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
    };

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };

    return vkCreateRenderPass(device, &render_pass_info, NULL, out_render_pass) == VK_SUCCESS;
}

void pb_rhi_offscreen_render_pass_destroy(pb_context *context, VkRenderPass render_pass)
{
    if (!context || render_pass == VK_NULL_HANDLE) {
        return;
    }

    vkDestroyRenderPass(pb_context_device(context), render_pass, NULL);
}

bool pb_rhi_offscreen_framebuffer_create(
    pb_context *context,
    VkRenderPass render_pass,
    VkImageView color_view,
    VkExtent2D extent,
    VkFramebuffer *out_framebuffer)
{
    if (!context || render_pass == VK_NULL_HANDLE || !color_view || !out_framebuffer ||
        extent.width == 0 || extent.height == 0) {
        return false;
    }

    VkFramebufferCreateInfo framebuffer_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = render_pass,
        .attachmentCount = 1,
        .pAttachments = &color_view,
        .width = extent.width,
        .height = extent.height,
        .layers = 1,
    };

    return vkCreateFramebuffer(
               pb_context_device(context),
               &framebuffer_info,
               NULL,
               out_framebuffer) == VK_SUCCESS;
}

void pb_rhi_offscreen_framebuffer_destroy(pb_context *context, VkFramebuffer framebuffer)
{
    if (!context || framebuffer == VK_NULL_HANDLE) {
        return;
    }

    vkDestroyFramebuffer(pb_context_device(context), framebuffer, NULL);
}

bool pb_rhi_offscreen_target_create(
    pb_context *context,
    VkFormat color_format,
    VkImageView color_view,
    VkExtent2D extent,
    pb_rhi_offscreen_target *target)
{
    if (!context || !color_view || !target || extent.width == 0 || extent.height == 0) {
        return false;
    }

    target_reset(target);
    target->color_view = color_view;
    target->extent = extent;

    if (!pb_rhi_offscreen_render_pass_create(context, color_format, &target->render_pass)) {
        target_reset(target);
        return false;
    }

    if (!pb_rhi_offscreen_framebuffer_create(
            context,
            target->render_pass,
            color_view,
            extent,
            &target->framebuffer)) {
        pb_rhi_offscreen_render_pass_destroy(context, target->render_pass);
        target_reset(target);
        return false;
    }

    return true;
}

void pb_rhi_offscreen_target_destroy(pb_context *context, pb_rhi_offscreen_target *target)
{
    if (!context || !target) {
        return;
    }

    pb_rhi_offscreen_framebuffer_destroy(context, target->framebuffer);
    pb_rhi_offscreen_render_pass_destroy(context, target->render_pass);
    target_reset(target);
}
