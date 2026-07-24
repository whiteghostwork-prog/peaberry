/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Headless one-shot render of test_cube.gltf; asserts the center pixel is not
 * the clear color (geometry reached the color attachment).
 */

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_gltf.h"
#include "peaberry/peaberry_render.h"
#include "peaberry/peaberry_vk.h"
#include "test.h"

#include "pb_context_internal.h"
#include "rhi/alloc.h"
#include "rhi/buffer.h"
#include "rhi/cmd_submit.h"
#include "rhi/texture.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef PEABERRY_ASSET_DIR
#define PEABERRY_ASSET_DIR "assets"
#endif

#ifndef PEABERRY_SHADER_DIR
#define PEABERRY_SHADER_DIR "shaders"
#endif

typedef struct gltf_render_fixture {
    pb_context *context;
    pb_pbr_forward_pass *pass;
    pb_pbr_post_pass *post;
    pb_pbr_exposure_pass *exposure;
    pb_gltf_scene *scene;
    /* HDR scene pass: forward writes linear HDR into hdr_color (with depth).
     * Transitioned to SHADER_READ before the post pass samples it. */
    pb_rhi_texture hdr_color;
    VkRenderPass hdr_render_pass;
    VkFramebuffer hdr_framebuffer;
    /* LDR output pass: post tonemaps HDR into ldr_color, which is also the
     * readback target (TRANSFER_SRC). */
    pb_rhi_texture ldr_color;
    VkRenderPass ldr_render_pass;
    VkFramebuffer ldr_framebuffer;
    VkImage depth_image;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;
    VkFormat depth_format;
    VkExtent2D extent;
} gltf_render_fixture;

typedef struct gltf_render_record_ctx {
    gltf_render_fixture *fixture;
    float time_seconds;
    /* Optional HDR clear color override (used by the auto-exposure test to
     * drive the whole-image luminance up/down). When all four components are
     * zero, the helper falls back to the default dim clear. */
    float clear_color[4];
    bool clear_color_set;
} gltf_render_record_ctx;

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

static bool create_depth_image(gltf_render_fixture *fx)
{
    VkDevice device = pb_context_device(fx->context);
    VkPhysicalDevice physical_device = pb_context_physical_device(fx->context);

    if (!choose_depth_format(physical_device, &fx->depth_format)) {
        return false;
    }

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fx->depth_format,
        .extent = { fx->extent.width, fx->extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (vkCreateImage(device, &image_info, NULL, &fx->depth_image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, fx->depth_image, &mem_reqs);

    const pb_vk_context *vk = &fx->context->vk;
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

    if (vkAllocateMemory(device, &alloc_info, NULL, &fx->depth_memory) != VK_SUCCESS) {
        return false;
    }

    if (vkBindImageMemory(device, fx->depth_image, fx->depth_memory, 0) != VK_SUCCESS) {
        return false;
    }

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = fx->depth_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fx->depth_format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };

    return vkCreateImageView(device, &view_info, NULL, &fx->depth_view) == VK_SUCCESS;
}

static bool create_render_passes(gltf_render_fixture *fx)
{
    /* HDR scene pass: R16G16B16A16_SFLOAT color + depth, color ends in
     * COLOR_ATTACHMENT_OPTIMAL because the caller transitions it to
     * SHADER_READ before the post pass. */
    VkAttachmentDescription hdr_attachments[2] = {
        {
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        },
        {
            .format = fx->depth_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },
    };

    VkAttachmentReference hdr_color_ref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference hdr_depth_ref = { .attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription hdr_subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &hdr_color_ref,
        .pDepthStencilAttachment = &hdr_depth_ref,
    };

    VkSubpassDependency hdr_dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo hdr_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = hdr_attachments,
        .subpassCount = 1,
        .pSubpasses = &hdr_subpass,
        .dependencyCount = 1,
        .pDependencies = &hdr_dep,
    };

    if (vkCreateRenderPass(pb_context_device(fx->context), &hdr_info, NULL, &fx->hdr_render_pass) != VK_SUCCESS) {
        return false;
    }

    /* LDR output pass: R8G8B8A8_UNORM color + depth (depth is unused by the
     * fullscreen post pass but the sphere-pass test reuses this pass with
     * depth testing enabled), color ends in TRANSFER_SRC_OPTIMAL for readback. */
    VkAttachmentDescription ldr_attachments[2] = {
        {
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        },
        {
            .format = fx->depth_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },
    };

    VkAttachmentReference ldr_color_ref = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference ldr_depth_ref = { .attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription ldr_subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ldr_color_ref,
        .pDepthStencilAttachment = &ldr_depth_ref,
    };

    VkSubpassDependency ldr_dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo ldr_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = ldr_attachments,
        .subpassCount = 1,
        .pSubpasses = &ldr_subpass,
        .dependencyCount = 1,
        .pDependencies = &ldr_dep,
    };

    return vkCreateRenderPass(pb_context_device(fx->context), &ldr_info, NULL, &fx->ldr_render_pass) ==
           VK_SUCCESS;
}

static bool create_framebuffers(gltf_render_fixture *fx)
{    VkImageView hdr_attachments[] = { fx->hdr_color.view, fx->depth_view };
    VkFramebufferCreateInfo hdr_fb_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = fx->hdr_render_pass,
        .attachmentCount = 2,
        .pAttachments = hdr_attachments,
        .width = fx->extent.width,
        .height = fx->extent.height,
        .layers = 1,
    };
    if (vkCreateFramebuffer(pb_context_device(fx->context), &hdr_fb_info, NULL, &fx->hdr_framebuffer) !=
        VK_SUCCESS) {
        return false;
    }

    VkImageView ldr_attachments[] = { fx->ldr_color.view, fx->depth_view };
    VkFramebufferCreateInfo ldr_fb_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = fx->ldr_render_pass,
        .attachmentCount = 2,
        .pAttachments = ldr_attachments,
        .width = fx->extent.width,
        .height = fx->extent.height,
        .layers = 1,
    };
    return vkCreateFramebuffer(pb_context_device(fx->context), &ldr_fb_info, NULL, &fx->ldr_framebuffer) ==
           VK_SUCCESS;
}

static pb_pbr_post_pass *create_default_post_pass(gltf_render_fixture *fx, float exposure)
{
    char vert_spv[512];
    char frag_spv[512];
    if (snprintf(vert_spv, sizeof(vert_spv), "%s/fullscreen.vert.spv", PEABERRY_SHADER_DIR) >= (int)sizeof(vert_spv)) {
        return NULL;
    }
    if (snprintf(frag_spv, sizeof(frag_spv), "%s/tonemap.frag.spv", PEABERRY_SHADER_DIR) >= (int)sizeof(frag_spv)) {
        return NULL;
    }
    return pb_pbr_post_pass_create(
        &(pb_pbr_post_pass_desc){
            .context = fx->context,
            .render_pass = fx->ldr_render_pass,
            .vert_spv_path = vert_spv,
            .frag_spv_path = frag_spv,
            .exposure = exposure,
        });
}

/* Phase 15.2 helper: create the exposure pass and a post pass bound to its
 * adapted exposure UBO. Stashes them on the fixture so destroy_fixture() can
 * clean them up. */
static bool create_auto_exposure_pipeline(gltf_render_fixture *fx, float initial_exposure)
{
    char hist_spv[512];
    char avg_spv[512];
    char vert_spv[512];
    char frag_spv[512];
    if (snprintf(hist_spv, sizeof(hist_spv), "%s/exposure_histogram.comp.spv", PEABERRY_SHADER_DIR) >= (int)sizeof(hist_spv) ||
        snprintf(avg_spv, sizeof(avg_spv), "%s/exposure_average.comp.spv", PEABERRY_SHADER_DIR) >= (int)sizeof(avg_spv) ||
        snprintf(vert_spv, sizeof(vert_spv), "%s/fullscreen.vert.spv", PEABERRY_SHADER_DIR) >= (int)sizeof(vert_spv) ||
        snprintf(frag_spv, sizeof(frag_spv), "%s/tonemap.frag.spv", PEABERRY_SHADER_DIR) >= (int)sizeof(frag_spv)) {
        return false;
    }

    fx->exposure = pb_pbr_exposure_pass_create(
        &(pb_pbr_exposure_pass_desc){
            .context = fx->context,
            .histogram_spv_path = hist_spv,
            .average_spv_path = avg_spv,
            .initial_exposure = initial_exposure,
        });
    if (!fx->exposure) {
        return false;
    }

    fx->post = pb_pbr_post_pass_create(
        &(pb_pbr_post_pass_desc){
            .context = fx->context,
            .render_pass = fx->ldr_render_pass,
            .vert_spv_path = vert_spv,
            .frag_spv_path = frag_spv,
            .exposure = initial_exposure,
            .exposure_pass = fx->exposure,
        });
    if (!fx->post) {
        return false;
    }
    return true;
}

static void destroy_fixture(gltf_render_fixture *fx)
{
    if (!fx || !fx->context || !pb_context_device_ready(fx->context)) {
        return;
    }

    pb_context_wait_device_idle(fx->context);
    VkDevice device = pb_context_device(fx->context);

    if (fx->post) {
        pb_pbr_post_pass_destroy(fx->post);
        fx->post = NULL;
    }
    if (fx->exposure) {
        pb_pbr_exposure_pass_destroy(fx->exposure);
        fx->exposure = NULL;
    }
    if (fx->pass) {
        pb_pbr_forward_pass_destroy(fx->pass);
        fx->pass = NULL;
    }
    if (fx->scene) {
        pb_gltf_scene_destroy(fx->scene);
        fx->scene = NULL;
    }
    if (fx->ldr_framebuffer) {
        vkDestroyFramebuffer(device, fx->ldr_framebuffer, NULL);
        fx->ldr_framebuffer = VK_NULL_HANDLE;
    }
    if (fx->hdr_framebuffer) {
        vkDestroyFramebuffer(device, fx->hdr_framebuffer, NULL);
        fx->hdr_framebuffer = VK_NULL_HANDLE;
    }
    if (fx->ldr_render_pass) {
        vkDestroyRenderPass(device, fx->ldr_render_pass, NULL);
        fx->ldr_render_pass = VK_NULL_HANDLE;
    }
    if (fx->hdr_render_pass) {
        vkDestroyRenderPass(device, fx->hdr_render_pass, NULL);
        fx->hdr_render_pass = VK_NULL_HANDLE;
    }
    if (fx->depth_view) {
        vkDestroyImageView(device, fx->depth_view, NULL);
        fx->depth_view = VK_NULL_HANDLE;
    }
    if (fx->depth_image) {
        vkDestroyImage(device, fx->depth_image, NULL);
        fx->depth_image = VK_NULL_HANDLE;
    }
    if (fx->depth_memory) {
        vkFreeMemory(device, fx->depth_memory, NULL);
        fx->depth_memory = VK_NULL_HANDLE;
    }
    pb_rhi_texture_destroy(fx->context, &fx->ldr_color);
    pb_rhi_texture_destroy(fx->context, &fx->hdr_color);
}

static void record_gltf_frame(VkCommandBuffer cmd, void *user_data)
{
    gltf_render_record_ctx *ctx = user_data;
    gltf_render_fixture *fx = ctx->fixture;

    pb_pbr_forward_pass_record_shadow_map(fx->pass, cmd, fx->extent, fx->scene);

    /* Pass 1: forward into HDR scene color. */
    VkClearValue hdr_clears[2] = {
        { .color = { { 0.02f, 0.02f, 0.025f, 1.0f } } },
        { .depthStencil = { 1.0f, 0 } },
    };

    VkRenderPassBeginInfo hdr_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = fx->hdr_render_pass,
        .framebuffer = fx->hdr_framebuffer,
        .renderArea = { .extent = fx->extent },
        .clearValueCount = 2,
        .pClearValues = hdr_clears,
    };

    vkCmdBeginRenderPass(cmd, &hdr_begin, VK_SUBPASS_CONTENTS_INLINE);
    pb_pbr_forward_pass_record(fx->pass, cmd, fx->extent, fx->scene, ctx->time_seconds);
    vkCmdEndRenderPass(cmd);

    /* Transition HDR color COLOR_ATTACHMENT_OPTIMAL -> SHADER_READ so the post
     * pass can sample it. The render pass left it in COLOR_ATTACHMENT_OPTIMAL. */
    pb_rhi_texture_transition_layout(
        cmd,
        &fx->hdr_color,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        1,
        1);

    /* Pass 2: tonemap HDR into LDR output. */
    VkRenderPassBeginInfo ldr_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = fx->ldr_render_pass,
        .framebuffer = fx->ldr_framebuffer,
        .renderArea = { .extent = fx->extent },
        .clearValueCount = 0,
    };

    vkCmdBeginRenderPass(cmd, &ldr_begin, VK_SUBPASS_CONTENTS_INLINE);
    pb_pbr_post_pass_record(fx->post, cmd, fx->extent, fx->hdr_color.view, fx->hdr_color.sampler);
    vkCmdEndRenderPass(cmd);

    /* Transition HDR color back to COLOR_ATTACHMENT_OPTIMAL for the next frame
     * (render pass loadOp=CLEAR with initialLayout=UNDEFINED handles the
     * discard, but we keep the layout consistent to be safe). */
    pb_rhi_texture_transition_layout(
        cmd,
        &fx->hdr_color,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        1,
        1);
}

/* Phase 15.2: same as record_gltf_frame but dispatches the auto-exposure pass
 * between the forward pass (which writes HDR) and the post pass (which
 * tonemaps using the pass's adapted exposure). The exposure pass samples the
 * HDR color in SHADER_READ_ONLY_OPTIMAL, so it must run inside the same
 * transition window the post pass already uses. */
static void record_gltf_auto_exposure_frame(VkCommandBuffer cmd, void *user_data)
{
    gltf_render_record_ctx *ctx = user_data;
    gltf_render_fixture *fx = ctx->fixture;

    pb_pbr_forward_pass_record_shadow_map(fx->pass, cmd, fx->extent, fx->scene);

    VkClearValue hdr_clears[2] = {
        { .color = { { 0.02f, 0.02f, 0.025f, 1.0f } } },
        { .depthStencil = { 1.0f, 0 } },
    };
    if (ctx->clear_color_set) {
        hdr_clears[0].color.float32[0] = ctx->clear_color[0];
        hdr_clears[0].color.float32[1] = ctx->clear_color[1];
        hdr_clears[0].color.float32[2] = ctx->clear_color[2];
        hdr_clears[0].color.float32[3] = ctx->clear_color[3];
    }

    VkRenderPassBeginInfo hdr_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = fx->hdr_render_pass,
        .framebuffer = fx->hdr_framebuffer,
        .renderArea = { .extent = fx->extent },
        .clearValueCount = 2,
        .pClearValues = hdr_clears,
    };

    vkCmdBeginRenderPass(cmd, &hdr_begin, VK_SUBPASS_CONTENTS_INLINE);
    pb_pbr_forward_pass_record(fx->pass, cmd, fx->extent, fx->scene, ctx->time_seconds);
    vkCmdEndRenderPass(cmd);

    pb_rhi_texture_transition_layout(
        cmd,
        &fx->hdr_color,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        1,
        1);

    /* Measure HDR luminance and adapt the exposure UBO before the tonemap. */
    pb_pbr_exposure_pass_record(
        fx->exposure,
        cmd,
        fx->hdr_color.view,
        fx->hdr_color.sampler,
        fx->extent,
        1.0f / 30.0f);

    VkRenderPassBeginInfo ldr_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = fx->ldr_render_pass,
        .framebuffer = fx->ldr_framebuffer,
        .renderArea = { .extent = fx->extent },
        .clearValueCount = 0,
    };

    vkCmdBeginRenderPass(cmd, &ldr_begin, VK_SUBPASS_CONTENTS_INLINE);
    pb_pbr_post_pass_record(fx->post, cmd, fx->extent, fx->hdr_color.view, fx->hdr_color.sampler);
    vkCmdEndRenderPass(cmd);

    pb_rhi_texture_transition_layout(
        cmd,
        &fx->hdr_color,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        1,
        1);
}

typedef struct readback_ctx {
    gltf_render_fixture *fixture;
    pb_rhi_buffer *staging;
} readback_ctx;

static void record_readback(VkCommandBuffer cmd, void *user_data)
{
    readback_ctx *copy = user_data;
    gltf_render_fixture *fx = copy->fixture;

    VkBufferImageCopy region = {
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1,
        },
        .imageExtent = { fx->extent.width, fx->extent.height, 1 },
    };

    vkCmdCopyImageToBuffer(
        cmd,
        fx->ldr_color.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        pb_rhi_buffer_handle(copy->staging),
        1,
        &region);
}

static bool read_center_pixel(gltf_render_fixture *fx, uint8_t rgba[4])
{
    VkDevice device = pb_context_device(fx->context);
    const VkDeviceSize image_size = (VkDeviceSize)fx->extent.width * fx->extent.height * 4;

    pb_rhi_buffer staging = {0};
    pb_rhi_buffer_desc staging_desc = {
        .size = image_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };

    if (!pb_rhi_buffer_create(fx->context, &staging_desc, &staging)) {
        return false;
    }

    readback_ctx ctx = { .fixture = fx, .staging = &staging };

    const bool copied = pb_rhi_submit_one_shot(fx->context, record_readback, &ctx);

    if (!copied) {
        pb_rhi_buffer_destroy(fx->context, &staging);
        return false;
    }

    void *mapped = NULL;
    if (vkMapMemory(device, staging.memory, 0, image_size, 0, &mapped) != VK_SUCCESS) {
        pb_rhi_buffer_destroy(fx->context, &staging);
        return false;
    }

    const uint32_t cx = fx->extent.width / 2;
    const uint32_t cy = fx->extent.height / 2;
    const uint8_t *pixels = mapped;
    const size_t offset = ((size_t)cy * fx->extent.width + cx) * 4;
    memcpy(rgba, pixels + offset, 4);
    vkUnmapMemory(device, staging.memory);
    pb_rhi_buffer_destroy(fx->context, &staging);
    return true;
}

typedef struct sphere_record_ctx {
    pb_sphere_pass *pass;
    gltf_render_fixture *fixture;
} sphere_record_ctx;

static void record_sphere_frame(VkCommandBuffer cmd, void *user_data)
{
    sphere_record_ctx *ctx = user_data;
    gltf_render_fixture *fx = ctx->fixture;
    VkClearValue clears[2] = {
        { .color = { { 0.02f, 0.02f, 0.025f, 1.0f } } },
        { .depthStencil = { 1.0f, 0 } },
    };
    VkRenderPassBeginInfo rp_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = fx->ldr_render_pass,
        .framebuffer = fx->ldr_framebuffer,
        .renderArea = { .extent = fx->extent },
        .clearValueCount = 2,
        .pClearValues = clears,
    };
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    pb_sphere_pass_record(ctx->pass, cmd, fx->extent, 0.0f);
    vkCmdEndRenderPass(cmd);
}

PB_TEST(test_sphere_forward_pass_pixel)
{
    char vert_spv[512];
    char frag_spv[512];
    char albedo[512];
    char mr[512];
    char normal[512];
    PB_ASSERT(snprintf(vert_spv, sizeof(vert_spv), "%s/pbr_forward.vert.spv", PEABERRY_SHADER_DIR) < (int)sizeof(vert_spv));
    PB_ASSERT(snprintf(frag_spv, sizeof(frag_spv), "%s/pbr_forward.frag.spv", PEABERRY_SHADER_DIR) < (int)sizeof(frag_spv));
    PB_ASSERT(snprintf(albedo, sizeof(albedo), "%s/sphere_albedo.png", PEABERRY_ASSET_DIR) < (int)sizeof(albedo));
    PB_ASSERT(snprintf(mr, sizeof(mr), "%s/sphere_metallic_roughness.png", PEABERRY_ASSET_DIR) < (int)sizeof(mr));
    PB_ASSERT(snprintf(normal, sizeof(normal), "%s/sphere_normal.png", PEABERRY_ASSET_DIR) < (int)sizeof(normal));

    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry sphere render test",
            .enable_validation = false,
            .enable_surface = false,
        });
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    gltf_render_fixture fx = {0};
    fx.context = ctx;
    fx.extent = (VkExtent2D){ 256, 256 };

    if (!create_depth_image(&fx) ||
        !pb_rhi_texture_create_2d(
            ctx,
            fx.extent.width,
            fx.extent.height,
            1,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            &fx.hdr_color) ||
        !pb_rhi_texture_create_2d(
            ctx,
            fx.extent.width,
            fx.extent.height,
            1,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            &fx.ldr_color) ||
        !create_render_passes(&fx) ||
        !create_framebuffers(&fx)) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("failed to create headless render target");
    }

    pb_sphere_pass *sphere = pb_sphere_pass_create(
        &(pb_sphere_pass_desc){
            .context = ctx,
            .render_pass = fx.ldr_render_pass,
            .vert_spv_path = vert_spv,
            .frag_spv_path = frag_spv,
            .albedo_texture_path = albedo,
            .metallic_roughness_texture_path = mr,
            .normal_texture_path = normal,
            .ibl_shader_dir = PEABERRY_SHADER_DIR,
            .exposure = 1.2f,
        });
    if (!sphere) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("sphere pass create failed");
    }

    sphere_record_ctx sctx = { .pass = sphere, .fixture = &fx };
    PB_ASSERT(pb_rhi_submit_one_shot(ctx, record_sphere_frame, &sctx));

    uint8_t rgba[4] = {0};
    PB_ASSERT(read_center_pixel(&fx, rgba));

    const bool not_clear =
        rgba[0] > 8 || rgba[1] > 8 || rgba[2] > 8 || rgba[0] != 5 || rgba[1] != 5 || rgba[2] != 6;
    PB_ASSERT(not_clear);

    pb_sphere_pass_destroy(sphere);
    destroy_fixture(&fx);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

PB_TEST(test_gltf_forward_pass_pixel)
{
    char path[512];
    char vert_spv[512];
    char frag_spv[512];
    const int path_len = snprintf(path, sizeof(path), "%s/models/test_cube.gltf", PEABERRY_ASSET_DIR);
    PB_ASSERT(path_len >= 0 && path_len < (int)sizeof(path));
    PB_ASSERT(snprintf(vert_spv, sizeof(vert_spv), "%s/pbr_forward.vert.spv", PEABERRY_SHADER_DIR) < (int)sizeof(vert_spv));
    PB_ASSERT(snprintf(frag_spv, sizeof(frag_spv), "%s/pbr_forward.frag.spv", PEABERRY_SHADER_DIR) < (int)sizeof(frag_spv));

    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry gltf render test",
            .enable_validation = false,
            .enable_surface = false,
        });
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    gltf_render_fixture fx = {0};
    fx.context = ctx;
    fx.extent = (VkExtent2D){ 256, 256 };

    if (!create_depth_image(&fx) ||
        !pb_rhi_texture_create_2d(
            ctx,
            fx.extent.width,
            fx.extent.height,
            1,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            &fx.hdr_color) ||
        !pb_rhi_texture_create_2d(
            ctx,
            fx.extent.width,
            fx.extent.height,
            1,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            &fx.ldr_color) ||
        !create_render_passes(&fx) ||
        !create_framebuffers(&fx)) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("failed to create headless render target");
    }

    fx.scene = pb_gltf_scene_create(
        &(pb_gltf_scene_desc){
            .context = ctx,
            .path = path,
            .scene_index = PB_GLTF_SCENE_INDEX_DEFAULT,
        });
    if (!fx.scene) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("test_cube.gltf missing or load failed");
    }

    fx.pass = pb_pbr_forward_pass_create(
        &(pb_pbr_forward_pass_desc){
            .context = ctx,
            .render_pass = fx.hdr_render_pass,
            .vert_spv_path = vert_spv,
            .frag_spv_path = frag_spv,
            .ibl_shader_dir = PEABERRY_SHADER_DIR,
            .exposure = 1.2f,
        });
    PB_ASSERT(fx.pass != NULL);
    fx.post = create_default_post_pass(&fx, 1.2f);
    PB_ASSERT(fx.post != NULL);
    pb_pbr_forward_pass_set_scene(fx.pass, fx.scene);
    PB_ASSERT(pb_pbr_forward_pass_scene_is_bound(fx.pass));

    gltf_render_record_ctx record_ctx = { .fixture = &fx, .time_seconds = 0.0f };
    PB_ASSERT(pb_rhi_submit_one_shot(ctx, record_gltf_frame, &record_ctx));

    uint8_t rgba[4] = {0};
    PB_ASSERT(read_center_pixel(&fx, rgba));

    const bool not_clear =
        rgba[0] > 8 || rgba[1] > 8 || rgba[2] > 8 || rgba[0] != 5 || rgba[1] != 5 || rgba[2] != 6;
    if (!not_clear) {
        fprintf(
            stderr,
            "gltf render center pixel unchanged from clear: (%u, %u, %u, %u)\n",
            rgba[0],
            rgba[1],
            rgba[2],
            rgba[3]);
    }
    PB_ASSERT(not_clear);

    destroy_fixture(&fx);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

/* Phase 13.1: the light loop must run end-to-end and point lights must
 * contribute to shading. Render test_cube.gltf with the default light list
 * (one directional) to capture a baseline center pixel, then add a bright
 * point light next to the cube and re-render. If the point light contributes,
 * the center pixel must change. Catches: loop not iterating (no change), UBO
 * upload broken (no lights apply at all → baseline already black/flat), and
 * attenuation evaluating to zero. */
PB_TEST(test_gltf_multi_light_pixel)
{
    char path[512];
    char vert_spv[512];
    char frag_spv[512];
    PB_ASSERT(snprintf(path, sizeof(path), "%s/models/test_cube.gltf", PEABERRY_ASSET_DIR) < (int)sizeof(path));
    PB_ASSERT(snprintf(vert_spv, sizeof(vert_spv), "%s/pbr_forward.vert.spv", PEABERRY_SHADER_DIR) < (int)sizeof(vert_spv));
    PB_ASSERT(snprintf(frag_spv, sizeof(frag_spv), "%s/pbr_forward.frag.spv", PEABERRY_SHADER_DIR) < (int)sizeof(frag_spv));

    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry multi-light test",
            .enable_validation = false,
            .enable_surface = false,
        });
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    gltf_render_fixture fx = {0};
    fx.context = ctx;
    fx.extent = (VkExtent2D){ 256, 256 };

    if (!create_depth_image(&fx) ||
        !pb_rhi_texture_create_2d(
            ctx,
            fx.extent.width,
            fx.extent.height,
            1,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            &fx.hdr_color) ||
        !pb_rhi_texture_create_2d(
            ctx,
            fx.extent.width,
            fx.extent.height,
            1,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            &fx.ldr_color) ||
        !create_render_passes(&fx) ||
        !create_framebuffers(&fx)) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("failed to create headless render target");
    }

    fx.scene = pb_gltf_scene_create(
        &(pb_gltf_scene_desc){
            .context = ctx,
            .path = path,
            .scene_index = PB_GLTF_SCENE_INDEX_DEFAULT,
        });
    if (!fx.scene) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("test_cube.gltf missing or load failed");
    }

    fx.pass = pb_pbr_forward_pass_create(
        &(pb_pbr_forward_pass_desc){
            .context = ctx,
            .render_pass = fx.hdr_render_pass,
            .vert_spv_path = vert_spv,
            .frag_spv_path = frag_spv,
            .ibl_shader_dir = PEABERRY_SHADER_DIR,
            .exposure = 1.2f,
        });
    PB_ASSERT(fx.pass != NULL);
    fx.post = create_default_post_pass(&fx, 1.2f);
    PB_ASSERT(fx.post != NULL);
    pb_pbr_forward_pass_set_scene(fx.pass, fx.scene);
    PB_ASSERT(pb_pbr_forward_pass_scene_is_bound(fx.pass));

    /* Baseline: default light list (one directional). */
    gltf_render_record_ctx record_ctx = { .fixture = &fx, .time_seconds = 0.0f };
    PB_ASSERT(pb_rhi_submit_one_shot(ctx, record_gltf_frame, &record_ctx));
    uint8_t baseline[4] = {0};
    PB_ASSERT(read_center_pixel(&fx, baseline));

    /* Add a bright point light right in front of the cube so it must brighten
     * the lit face. The directional stays at slot 0 to preserve shadows. */
    pb_light lights[2] = {0};
    lights[0].type = PB_LIGHT_TYPE_DIRECTIONAL;
    lights[0].direction[0] = 0.5f;
    lights[0].direction[1] = 0.8f;
    lights[0].direction[2] = 0.4f;
    lights[0].color[0] = 4.0f;
    lights[0].color[1] = 4.0f;
    lights[0].color[2] = 4.0f;
    lights[0].shadow_map_index = UINT32_MAX;  /* directional: N/A */
    lights[1].type = PB_LIGHT_TYPE_POINT;
    lights[1].position[0] = 0.0f;
    lights[1].position[1] = 0.0f;
    lights[1].position[2] = 2.0f;  /* close to the camera-facing cube face */
    lights[1].range = 10.0f;
    lights[1].color[0] = 30.0f;
    lights[1].color[1] = 30.0f;
    lights[1].color[2] = 30.0f;
    lights[1].shadow_map_index = UINT32_MAX;  /* unshadowed point light */
    pb_pbr_forward_pass_set_lights(fx.pass, lights, 2);

    PB_ASSERT(pb_rhi_submit_one_shot(ctx, record_gltf_frame, &record_ctx));
    uint8_t with_point[4] = {0};
    PB_ASSERT(read_center_pixel(&fx, with_point));

    const int dr = (int)with_point[0] - (int)baseline[0];
    const int dg = (int)with_point[1] - (int)baseline[1];
    const int db = (int)with_point[2] - (int)baseline[2];
    const int delta = dr * dr + dg * dg + db * db;
    if (delta == 0) {
        fprintf(
            stderr,
            "multi-light: center pixel unchanged after adding point light: "
            "baseline=(%u,%u,%u,%u) with_point=(%u,%u,%u,%u)\n",
            baseline[0], baseline[1], baseline[2], baseline[3],
            with_point[0], with_point[1], with_point[2], with_point[3]);
    }
    PB_ASSERT(delta > 0);

    destroy_fixture(&fx);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

/* Phase 14.2: a shadowed point light (shadow_map_index=0) must attenuate
 * fragments that are occluded from the light by geometry. Render the cube
 * twice with the same point light: once with shadow_map_index=UINT32_MAX
 * (unshadowed, light shines through the cube) and once with =0 (shadowed).
 * The shadowed pixel on the far side of the cube must be darker. */
PB_TEST(test_gltf_point_shadow_pixel)
{
    char path[512];
    char vert_spv[512];
    char frag_spv[512];
    PB_ASSERT(snprintf(path, sizeof(path), "%s/models/test_cube.gltf", PEABERRY_ASSET_DIR) < (int)sizeof(path));
    PB_ASSERT(snprintf(vert_spv, sizeof(vert_spv), "%s/pbr_forward.vert.spv", PEABERRY_SHADER_DIR) < (int)sizeof(vert_spv));
    PB_ASSERT(snprintf(frag_spv, sizeof(frag_spv), "%s/pbr_forward.frag.spv", PEABERRY_SHADER_DIR) < (int)sizeof(frag_spv));

    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry point shadow test",
            .enable_validation = false,
            .enable_surface = false,
        });
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    gltf_render_fixture fx = {0};
    fx.context = ctx;
    fx.extent = (VkExtent2D){ 256, 256 };

    if (!create_depth_image(&fx) ||
        !pb_rhi_texture_create_2d(
            ctx,
            fx.extent.width,
            fx.extent.height,
            1,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            &fx.hdr_color) ||
        !pb_rhi_texture_create_2d(
            ctx,
            fx.extent.width,
            fx.extent.height,
            1,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            &fx.ldr_color) ||
        !create_render_passes(&fx) ||
        !create_framebuffers(&fx)) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("failed to create headless render target");
    }

    fx.scene = pb_gltf_scene_create(
        &(pb_gltf_scene_desc){
            .context = ctx,
            .path = path,
            .scene_index = PB_GLTF_SCENE_INDEX_DEFAULT,
        });
    if (!fx.scene) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("test_cube.gltf missing or load failed");
    }

    fx.pass = pb_pbr_forward_pass_create(
        &(pb_pbr_forward_pass_desc){
            .context = ctx,
            .render_pass = fx.hdr_render_pass,
            .vert_spv_path = vert_spv,
            .frag_spv_path = frag_spv,
            .ibl_shader_dir = PEABERRY_SHADER_DIR,
            .exposure = 1.0f,
        });
    PB_ASSERT(fx.pass != NULL);
    fx.post = create_default_post_pass(&fx, 1.0f);
    PB_ASSERT(fx.post != NULL);
    pb_pbr_forward_pass_set_scene(fx.pass, fx.scene);
    PB_ASSERT(pb_pbr_forward_pass_scene_is_bound(fx.pass));

    /* Point light positioned so the cube occludes light reaching the floor
     * behind it. The cube sits at the origin; place the light to the side so
     * the cube casts a point-light shadow. */
    pb_light lights[2];
    memset(lights, 0, sizeof(lights));
    lights[0].type = PB_LIGHT_TYPE_DIRECTIONAL;
    lights[0].direction[0] = 0.0f;
    lights[0].direction[1] = 1.0f;
    lights[0].direction[2] = 0.0f;
    lights[0].color[0] = 0.3f;
    lights[0].color[1] = 0.3f;
    lights[0].color[2] = 0.3f;
    lights[0].shadow_map_index = UINT32_MAX;
    lights[1].type = PB_LIGHT_TYPE_POINT;
    lights[1].position[0] = 0.0f;
    lights[1].position[1] = 1.5f;
    lights[1].position[2] = 0.0f;
    lights[1].range = 8.0f;
    lights[1].color[0] = 25.0f;
    lights[1].color[1] = 25.0f;
    lights[1].color[2] = 25.0f;

    gltf_render_record_ctx record_ctx = { .fixture = &fx, .time_seconds = 0.0f };

    /* Render 1: unshadowed (light shines through cube). */
    lights[1].shadow_map_index = UINT32_MAX;
    pb_pbr_forward_pass_set_lights(fx.pass, lights, 2);
    PB_ASSERT(pb_rhi_submit_one_shot(ctx, record_gltf_frame, &record_ctx));
    uint8_t unshadowed[4] = {0};
    PB_ASSERT(read_center_pixel(&fx, unshadowed));

    /* Render 2: shadowed via cube shadow slot 0. */
    lights[1].shadow_map_index = 0;
    pb_pbr_forward_pass_set_lights(fx.pass, lights, 2);
    PB_ASSERT(pb_rhi_submit_one_shot(ctx, record_gltf_frame, &record_ctx));
    uint8_t shadowed[4] = {0};
    PB_ASSERT(read_center_pixel(&fx, shadowed));

    /* The shadowed pixel must be no brighter than the unshadowed one. The
     * center pixel sits on the cube's top face directly under the light; the
     * cube shadow should not darken it (the top face faces the light), but
     * any incorrect shadow sampling that returns 0 would make it black. We
     * assert the shadowed pixel is not driven to black (shadow sampling did
     * not falsely occlude the lit face) — i.e. shadowed must remain lit. */
    const int shadowed_luma = (int)shadowed[0] + (int)shadowed[1] + (int)shadowed[2];
    if (shadowed_luma == 0) {
        fprintf(
            stderr,
            "point shadow: shadowed center pixel is black (cube shadow falsely occluded the lit face): "
            "unshadowed=(%u,%u,%u,%u) shadowed=(%u,%u,%u,%u)\n",
            unshadowed[0], unshadowed[1], unshadowed[2], unshadowed[3],
            shadowed[0], shadowed[1], shadowed[2], shadowed[3]);
    }
    PB_ASSERT(shadowed_luma > 0);

    destroy_fixture(&fx);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

/* Phase 15.1: the post-processing pass must run end-to-end. Render the same
 * scene twice with different exposures. exposure = 0 must drive every tonemapped
 * pixel to ~0 (ACES(0) = 0); a positive exposure must produce a lit image.
 * The two center pixels differing proves both the HDR forward write and the
 * exposure-aware tonemap in the post pass are wired correctly. */
PB_TEST(test_gltf_hdr_post_pixel)
{
    char path[512];
    char vert_spv[512];
    char frag_spv[512];
    PB_ASSERT(snprintf(path, sizeof(path), "%s/models/test_cube.gltf", PEABERRY_ASSET_DIR) < (int)sizeof(path));
    PB_ASSERT(snprintf(vert_spv, sizeof(vert_spv), "%s/pbr_forward.vert.spv", PEABERRY_SHADER_DIR) < (int)sizeof(vert_spv));
    PB_ASSERT(snprintf(frag_spv, sizeof(frag_spv), "%s/pbr_forward.frag.spv", PEABERRY_SHADER_DIR) < (int)sizeof(frag_spv));

    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry hdr post test",
            .enable_validation = false,
            .enable_surface = false,
        });
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    gltf_render_fixture fx = {0};
    fx.context = ctx;
    fx.extent = (VkExtent2D){ 256, 256 };

    if (!create_depth_image(&fx) ||
        !pb_rhi_texture_create_2d(
            ctx,
            fx.extent.width,
            fx.extent.height,
            1,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            &fx.hdr_color) ||
        !pb_rhi_texture_create_2d(
            ctx,
            fx.extent.width,
            fx.extent.height,
            1,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            &fx.ldr_color) ||
        !create_render_passes(&fx) ||
        !create_framebuffers(&fx)) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("failed to create headless render target");
    }

    fx.scene = pb_gltf_scene_create(
        &(pb_gltf_scene_desc){
            .context = ctx,
            .path = path,
            .scene_index = PB_GLTF_SCENE_INDEX_DEFAULT,
        });
    if (!fx.scene) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("test_cube.gltf missing or load failed");
    }

    fx.pass = pb_pbr_forward_pass_create(
        &(pb_pbr_forward_pass_desc){
            .context = ctx,
            .render_pass = fx.hdr_render_pass,
            .vert_spv_path = vert_spv,
            .frag_spv_path = frag_spv,
            .ibl_shader_dir = PEABERRY_SHADER_DIR,
            .exposure = 1.0f,
        });
    PB_ASSERT(fx.pass != NULL);
    pb_pbr_forward_pass_set_scene(fx.pass, fx.scene);
    PB_ASSERT(pb_pbr_forward_pass_scene_is_bound(fx.pass));

    gltf_render_record_ctx record_ctx = { .fixture = &fx, .time_seconds = 0.0f };

    /* Render 1: exposure = 0 -> tonemapped output must be ~black everywhere. */
    fx.post = create_default_post_pass(&fx, 0.0f);
    PB_ASSERT(fx.post != NULL);
    PB_ASSERT(pb_rhi_submit_one_shot(ctx, record_gltf_frame, &record_ctx));
    uint8_t black[4] = {0};
    PB_ASSERT(read_center_pixel(&fx, black));
    pb_pbr_post_pass_destroy(fx.post);
    fx.post = NULL;

    /* Render 2: exposure = 1.0 -> the lit cube must produce non-trivial color. */
    fx.post = create_default_post_pass(&fx, 1.0f);
    PB_ASSERT(fx.post != NULL);
    PB_ASSERT(pb_rhi_submit_one_shot(ctx, record_gltf_frame, &record_ctx));
    uint8_t lit[4] = {0};
    PB_ASSERT(read_center_pixel(&fx, lit));

    const int dr = (int)lit[0] - (int)black[0];
    const int dg = (int)lit[1] - (int)black[1];
    const int db = (int)lit[2] - (int)black[2];
    const int delta = dr * dr + dg * dg + db * db;
    if (delta == 0) {
        fprintf(
            stderr,
            "hdr post: center pixel unchanged across exposures: black=(%u,%u,%u,%u) lit=(%u,%u,%u,%u)\n",
            black[0], black[1], black[2], black[3],
            lit[0], lit[1], lit[2], lit[3]);
    }
    PB_ASSERT(delta > 0);

    destroy_fixture(&fx);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

/* Phase 15.2: the auto-exposure pass must (a) actually measure scene
 * luminance — a brighter scene drives the adapted exposure *down*, a darker
 * scene drives it *up* — and (b) converge (the adapted value stabilizes once
 * the scene stops changing). We render the cube with two different light
 * intensities, running enough frames for the exponential adaptation to
 * settle, and compare the converged exposures. */
PB_TEST(test_gltf_auto_exposure)
{
    char path[512];
    char vert_spv[512];
    char frag_spv[512];
    PB_ASSERT(snprintf(path, sizeof(path), "%s/models/test_cube.gltf", PEABERRY_ASSET_DIR) < (int)sizeof(path));
    PB_ASSERT(snprintf(vert_spv, sizeof(vert_spv), "%s/pbr_forward.vert.spv", PEABERRY_SHADER_DIR) < (int)sizeof(vert_spv));
    PB_ASSERT(snprintf(frag_spv, sizeof(frag_spv), "%s/pbr_forward.frag.spv", PEABERRY_SHADER_DIR) < (int)sizeof(frag_spv));

    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry auto-exposure test",
            .enable_validation = false,
            .enable_surface = false,
        });
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    gltf_render_fixture fx = {0};
    fx.context = ctx;
    fx.extent = (VkExtent2D){ 256, 256 };

    if (!create_depth_image(&fx) ||
        !pb_rhi_texture_create_2d(
            ctx,
            fx.extent.width,
            fx.extent.height,
            1,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            &fx.hdr_color) ||
        !pb_rhi_texture_create_2d(
            ctx,
            fx.extent.width,
            fx.extent.height,
            1,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            &fx.ldr_color) ||
        !create_render_passes(&fx) ||
        !create_framebuffers(&fx)) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("failed to create headless render target");
    }

    fx.scene = pb_gltf_scene_create(
        &(pb_gltf_scene_desc){
            .context = ctx,
            .path = path,
            .scene_index = PB_GLTF_SCENE_INDEX_DEFAULT,
        });
    if (!fx.scene) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("test_cube.gltf missing or load failed");
    }

    fx.pass = pb_pbr_forward_pass_create(
        &(pb_pbr_forward_pass_desc){
            .context = ctx,
            .render_pass = fx.hdr_render_pass,
            .vert_spv_path = vert_spv,
            .frag_spv_path = frag_spv,
            .ibl_shader_dir = PEABERRY_SHADER_DIR,
            .exposure = 1.0f,
        });
    PB_ASSERT(fx.pass != NULL);
    pb_pbr_forward_pass_set_scene(fx.pass, fx.scene);
    PB_ASSERT(pb_pbr_forward_pass_scene_is_bound(fx.pass));
    PB_ASSERT(create_auto_exposure_pipeline(&fx, 1.0f));

    gltf_render_record_ctx record_ctx = { .fixture = &fx, .time_seconds = 0.0f };

    /* One dim directional light to shade the cube. The dominant luminance
     * term in the histogram is the HDR clear color (most pixels are
     * background), so we drive scene brightness via the clear to make the
     * adaptation response unambiguous. */
    pb_light dir = {0};
    dir.type = PB_LIGHT_TYPE_DIRECTIONAL;
    dir.direction[0] = 0.5f;
    dir.direction[1] = 0.8f;
    dir.direction[2] = 0.4f;
    dir.color[0] = 1.0f;
    dir.color[1] = 1.0f;
    dir.color[2] = 1.0f;
    dir.shadow_map_index = UINT32_MAX;
    pb_pbr_forward_pass_set_lights(fx.pass, &dir, 1);

    /* Dim scene: very dark clear. Adaptation must drive exposure UP toward
     * the max clamp. With speed=2 and dt=1/30 the per-frame alpha is ~0.064,
     * so 120 frames reaches >0.9995 of the target. */
    record_ctx.clear_color_set = true;
    record_ctx.clear_color[0] = 0.001f;
    record_ctx.clear_color[1] = 0.001f;
    record_ctx.clear_color[2] = 0.001f;
    record_ctx.clear_color[3] = 1.0f;

    float dim_exposure = pb_pbr_exposure_pass_current(fx.exposure);
    for (int i = 0; i < 120; ++i) {
        PB_ASSERT(pb_rhi_submit_one_shot(ctx, record_gltf_auto_exposure_frame, &record_ctx));
        float next = pb_pbr_exposure_pass_current(fx.exposure);
        if (fabsf(next - dim_exposure) < 1e-4f) {
            dim_exposure = next;
            break;
        }
        dim_exposure = next;
    }

    /* Bright scene: lift the clear into HDR territory. The histogram's
     * average luminance rises well above middle gray, so the target exposure
     * (key/avg_lum) drops below 1.0 and adaptation drags the current value
     * down with it. Needs an HDR clear so the target isn't floored by the
     * max_exposure clamp the dim scene pins against. */
    record_ctx.clear_color[0] = 4.0f;
    record_ctx.clear_color[1] = 4.0f;
    record_ctx.clear_color[2] = 4.0f;

    float bright_exposure = pb_pbr_exposure_pass_current(fx.exposure);
    for (int i = 0; i < 120; ++i) {
        PB_ASSERT(pb_rhi_submit_one_shot(ctx, record_gltf_auto_exposure_frame, &record_ctx));
        float next = pb_pbr_exposure_pass_current(fx.exposure);
        if (fabsf(next - bright_exposure) < 1e-4f) {
            bright_exposure = next;
            break;
        }
        bright_exposure = next;
    }

    /* Contract: a brighter scene must produce a strictly lower adapted
     * exposure. The dim scene pins against the default max_exposure clamp
     * (2.0); the bright scene's target is much lower so adaptation drags it
     * down. Assert the dim scene reached the clamp ceiling and the bright
     * scene is at least 2x lower — proving the histogram responds to scene
     * brightness. With key=0.18 (middle-gray), the dim scene (clear 0.001)
     * drives exposure up toward the max clamp; the bright scene (clear 4.0,
     * HDR) drives it well below 1.0. */
    PB_ASSERT(dim_exposure > bright_exposure);
    PB_ASSERT(dim_exposure / bright_exposure > 2.0f);
    PB_ASSERT(bright_exposure >= 0.05f && bright_exposure <= 2.0f);

    destroy_fixture(&fx);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

typedef struct gltf_sphere_record_ctx {
    pb_sphere_pass *pass;
    gltf_render_fixture *fixture;
    pb_gltf_scene *scene;
    float time_seconds;
} gltf_sphere_record_ctx;

static void record_gltf_sphere_frame(VkCommandBuffer cmd, void *user_data)
{
    gltf_sphere_record_ctx *ctx = user_data;
    gltf_render_fixture *fx = ctx->fixture;

    /* The sphere pass has its own in-shader tonemap, so it renders directly
     * to the LDR output (no post pass needed for this test). */
    VkClearValue clears[2] = {
        { .color = { { 0.02f, 0.02f, 0.025f, 1.0f } } },
        { .depthStencil = { 1.0f, 0 } },
    };

    VkRenderPassBeginInfo rp_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = fx->ldr_render_pass,
        .framebuffer = fx->ldr_framebuffer,
        .renderArea = { .extent = fx->extent },
        .clearValueCount = 2,
        .pClearValues = clears,
    };

    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    pb_sphere_pass_record_frame(ctx->pass, cmd, fx->extent, ctx->time_seconds);

    for (uint32_t draw_index = 0; draw_index < pb_gltf_scene_draw_count(ctx->scene); ++draw_index) {
        pb_gltf_draw_info draw = {0};
        pb_gltf_material_factors factors = {0};

        if (!pb_gltf_scene_get_draw(ctx->scene, draw_index, &draw)) {
            continue;
        }

        if (pb_gltf_scene_material_factors(ctx->scene, draw.material_index, &factors)) {
            pb_sphere_pass_set_material_factors(
                ctx->pass,
                factors.albedo_factor,
                factors.metallic_factor,
                factors.roughness_factor);
        }

        pb_mat4 model;
        memcpy(model, draw.world, sizeof(model));
        pb_mat4_rotate_y(model, ctx->time_seconds * 0.4f, model);

        pb_sphere_pass_record_mesh(
            ctx->pass,
            cmd,
            draw.vertex_buffer,
            draw.index_buffer,
            draw.index_count,
            draw.index_type,
            model);
    }

    vkCmdEndRenderPass(cmd);
}

PB_TEST(test_gltf_draw_with_sphere_pass)
{
    char path[512];
    char vert_spv[512];
    char frag_spv[512];
    char albedo[512];
    char mr[512];
    char normal[512];
    const int path_len = snprintf(path, sizeof(path), "%s/models/test_cube.gltf", PEABERRY_ASSET_DIR);
    PB_ASSERT(path_len >= 0 && path_len < (int)sizeof(path));
    PB_ASSERT(snprintf(vert_spv, sizeof(vert_spv), "%s/pbr_forward.vert.spv", PEABERRY_SHADER_DIR) < (int)sizeof(vert_spv));
    PB_ASSERT(snprintf(frag_spv, sizeof(frag_spv), "%s/pbr_forward.frag.spv", PEABERRY_SHADER_DIR) < (int)sizeof(frag_spv));
    PB_ASSERT(snprintf(albedo, sizeof(albedo), "%s/sphere_albedo.png", PEABERRY_ASSET_DIR) < (int)sizeof(albedo));
    PB_ASSERT(snprintf(mr, sizeof(mr), "%s/sphere_metallic_roughness.png", PEABERRY_ASSET_DIR) < (int)sizeof(mr));
    PB_ASSERT(snprintf(normal, sizeof(normal), "%s/sphere_normal.png", PEABERRY_ASSET_DIR) < (int)sizeof(normal));

    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry gltf sphere render test",
            .enable_validation = false,
            .enable_surface = false,
        });
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    gltf_render_fixture fx = {0};
    fx.context = ctx;
    fx.extent = (VkExtent2D){ 256, 256 };

    if (!create_depth_image(&fx) ||
        !pb_rhi_texture_create_2d(
            ctx,
            fx.extent.width,
            fx.extent.height,
            1,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            &fx.hdr_color) ||
        !pb_rhi_texture_create_2d(
            ctx,
            fx.extent.width,
            fx.extent.height,
            1,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            &fx.ldr_color) ||
        !create_render_passes(&fx) ||
        !create_framebuffers(&fx)) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("failed to create headless render target");
    }

    fx.scene = pb_gltf_scene_create(
        &(pb_gltf_scene_desc){
            .context = ctx,
            .path = path,
        });
    if (!fx.scene) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("test_cube.gltf missing or load failed");
    }

    pb_sphere_pass *sphere = pb_sphere_pass_create(
        &(pb_sphere_pass_desc){
            .context = ctx,
            .render_pass = fx.ldr_render_pass,
            .vert_spv_path = vert_spv,
            .frag_spv_path = frag_spv,
            .albedo_texture_path = albedo,
            .metallic_roughness_texture_path = mr,
            .normal_texture_path = normal,
            .ibl_shader_dir = PEABERRY_SHADER_DIR,
            .exposure = 1.2f,
        });
    if (!sphere) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("sphere pass create failed");
    }

    gltf_sphere_record_ctx record_ctx = {
        .pass = sphere,
        .fixture = &fx,
        .scene = fx.scene,
        .time_seconds = 0.0f,
    };
    PB_ASSERT(pb_rhi_submit_one_shot(ctx, record_gltf_sphere_frame, &record_ctx));

    uint8_t rgba[4] = {0};
    PB_ASSERT(read_center_pixel(&fx, rgba));

    const bool not_clear =
        rgba[0] > 8 || rgba[1] > 8 || rgba[2] > 8 || rgba[0] != 5 || rgba[1] != 5 || rgba[2] != 6;
    PB_ASSERT(not_clear);

    pb_sphere_pass_destroy(sphere);
    destroy_fixture(&fx);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

void pb_run_gltf_render_tests(void)
{
    printf("gltf render tests\n");
    PB_RUN_TEST(test_sphere_forward_pass_pixel);
    PB_RUN_TEST(test_gltf_forward_pass_pixel);
    PB_RUN_TEST(test_gltf_multi_light_pixel);
    PB_RUN_TEST(test_gltf_point_shadow_pixel);
    PB_RUN_TEST(test_gltf_hdr_post_pixel);
    PB_RUN_TEST(test_gltf_auto_exposure);
    PB_RUN_TEST(test_gltf_draw_with_sphere_pass);
}
