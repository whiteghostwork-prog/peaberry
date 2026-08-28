/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Headless render-to-PNG diagnostic. Renders a glTF scene with the same HDR
 * two-pass pipeline (forward + auto-exposure + tonemap) the viewer uses, and
 * writes the LDR output as a PNG so visual artifacts can be inspected without
 * a windowing system.
 *
 * Usage: peaberry_capture <model.gltf> <output.png> [width] [height]
 */

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_gltf.h"
#include "peaberry/peaberry_vk.h"
#include "pb_context_internal.h"
#include "rhi/buffer.h"
#include "rhi/cmd_submit.h"
#include "rhi/texture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PEABERRY_SHADER_DIR
#define PEABERRY_SHADER_DIR "shaders"
#endif

typedef struct {
    pb_context *context;
    pb_pbr_forward_pass *pass;
    pb_pbr_post_pass *post;
    pb_pbr_exposure_pass *exposure;
    pb_gltf_scene *scene;
    pb_rhi_texture hdr_color;
    VkRenderPass hdr_render_pass;
    VkFramebuffer hdr_framebuffer;
    pb_rhi_texture ldr_color;
    VkRenderPass ldr_render_pass;
    VkFramebuffer ldr_framebuffer;
    VkImage depth_image;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;
    VkFormat depth_format;
    VkExtent2D extent;
} fixture;

static bool choose_depth_format(VkPhysicalDevice pd, VkFormat *out)
{
    const VkFormat cands[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT };
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); ++i) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(pd, cands[i], &p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            *out = cands[i];
            return true;
        }
    }
    return false;
}

static bool make_render_passes(fixture *fx)
{
    VkDevice dev = pb_context_device(fx->context);
    VkAttachmentDescription hdr[] = {
        { .format = VK_FORMAT_R16G16B16A16_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
        { .format = fx->depth_format, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
    };
    VkAttachmentReference hdr_col = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference hdr_dep = { .attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription hdr_sp = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &hdr_col,
        .pDepthStencilAttachment = &hdr_dep,
    };
    VkRenderPassCreateInfo hdr_i = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2, .pAttachments = hdr,
        .subpassCount = 1, .pSubpasses = &hdr_sp,
    };
    if (vkCreateRenderPass(dev, &hdr_i, NULL, &fx->hdr_render_pass) != VK_SUCCESS) return false;

    VkAttachmentDescription ldr[] = {
        { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL },
    };
    VkAttachmentReference ldr_col = { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription ldr_sp = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ldr_col,
    };
    VkRenderPassCreateInfo ldr_i = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = ldr,
        .subpassCount = 1, .pSubpasses = &ldr_sp,
    };
    return vkCreateRenderPass(dev, &ldr_i, NULL, &fx->ldr_render_pass) == VK_SUCCESS;
}

static bool make_framebuffers(fixture *fx)
{
    VkDevice dev = pb_context_device(fx->context);
    VkImageView hdr_atts[] = { fx->hdr_color.view, fx->depth_view };
    VkFramebufferCreateInfo hdr_fb = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = fx->hdr_render_pass, .attachmentCount = 2, .pAttachments = hdr_atts,
        .width = fx->extent.width, .height = fx->extent.height, .layers = 1,
    };
    if (vkCreateFramebuffer(dev, &hdr_fb, NULL, &fx->hdr_framebuffer) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo ldr_fb = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = fx->ldr_render_pass, .attachmentCount = 1, .pAttachments = &fx->ldr_color.view,
        .width = fx->extent.width, .height = fx->extent.height, .layers = 1,
    };
    return vkCreateFramebuffer(dev, &ldr_fb, NULL, &fx->ldr_framebuffer) == VK_SUCCESS;
}

static bool make_depth(fixture *fx)
{
    VkDevice dev = pb_context_device(fx->context);
    if (!choose_depth_format(pb_context_physical_device(fx->context), &fx->depth_format)) return false;
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = fx->depth_format,
        .extent = { fx->extent.width, fx->extent.height, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev, &ii, NULL, &fx->depth_image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, fx->depth_image, &mr);
    const pb_vk_context *vk = &fx->context->vk;
    uint32_t mt = pb_rhi_find_memory_type(vk, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = mt,
    };
    if (vkAllocateMemory(dev, &ai, NULL, &fx->depth_memory) != VK_SUCCESS) return false;
    if (vkBindImageMemory(dev, fx->depth_image, fx->depth_memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = fx->depth_image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fx->depth_format,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .levelCount = 1, .layerCount = 1 },
    };
    return vkCreateImageView(dev, &vi, NULL, &fx->depth_view) == VK_SUCCESS;
}

static void record_frame(VkCommandBuffer cmd, void *ud)
{
    fixture *fx = ud;
    pb_pbr_forward_pass_record_shadow_map(fx->pass, cmd, fx->extent, fx->scene);

    VkClearValue hdr_clears[2] = {
        { .color = { { 0.02f, 0.02f, 0.025f, 1.0f } } },
        { .depthStencil = { 1.0f, 0 } },
    };
    VkRenderPassBeginInfo hdr_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = fx->hdr_render_pass, .framebuffer = fx->hdr_framebuffer,
        .renderArea = { .extent = fx->extent }, .clearValueCount = 2, .pClearValues = hdr_clears,
    };
    vkCmdBeginRenderPass(cmd, &hdr_begin, VK_SUBPASS_CONTENTS_INLINE);
    pb_pbr_forward_pass_record(fx->pass, cmd, fx->extent, fx->scene, 0.0f);
    vkCmdEndRenderPass(cmd);

    pb_rhi_texture_transition_layout(cmd, &fx->hdr_color,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 1);

    if (fx->exposure) {
        pb_pbr_exposure_pass_record(fx->exposure, cmd, fx->hdr_color.view, fx->hdr_color.sampler,
            fx->extent, 1.0f / 30.0f);
    }

    VkRenderPassBeginInfo ldr_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = fx->ldr_render_pass, .framebuffer = fx->ldr_framebuffer,
        .renderArea = { .extent = fx->extent }, .clearValueCount = 0,
    };
    vkCmdBeginRenderPass(cmd, &ldr_begin, VK_SUBPASS_CONTENTS_INLINE);
    pb_pbr_post_pass_record(fx->post, cmd, fx->extent, fx->hdr_color.view, fx->hdr_color.sampler);
    vkCmdEndRenderPass(cmd);

    pb_rhi_texture_transition_layout(cmd, &fx->hdr_color,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, 1);
}

typedef struct {
    fixture *fx;
    pb_rhi_buffer *staging;
} readback_ctx;

static void record_readback(VkCommandBuffer cmd, void *ud)
{
    readback_ctx *rc = ud;
    VkBufferImageCopy region = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { rc->fx->extent.width, rc->fx->extent.height, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, rc->fx->ldr_color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        pb_rhi_buffer_handle(rc->staging), 1, &region);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <model.gltf> <output.png> [width] [height]\n", argv[0]);
        return 2;
    }
    const char *model = argv[1];
    const char *out = argv[2];
    uint32_t w = argc > 3 ? (uint32_t)atoi(argv[3]) : 512;
    uint32_t h = argc > 4 ? (uint32_t)atoi(argv[4]) : 512;
    /* Optional flags after the dimensions: "noexpo" disables auto-exposure
     * (uses a static exposure of 1.0), "noshadow" disables the shadow pass,
     * "shadowdebug" paints the shadow factor as a brightness modulation. */
    bool use_auto_exposure = true;
    bool use_shadows = true;
    bool use_shadow_debug = false;
    bool use_depth_viz = false;
    for (int a = 5; a < argc; ++a) {
        if (strcmp(argv[a], "noexpo") == 0) use_auto_exposure = false;
        if (strcmp(argv[a], "noshadow") == 0) use_shadows = false;
        if (strcmp(argv[a], "shadowdebug") == 0) use_shadow_debug = true;
        if (strcmp(argv[a], "depthviz") == 0) use_depth_viz = true;
    }

    pb_context *ctx = pb_context_create(&(pb_context_desc){
        .app_name = "peaberry_capture", .enable_validation = false, .enable_surface = false });
    if (!ctx || !pb_context_init_headless_device(ctx)) {
        fprintf(stderr, "no Vulkan device\n");
        return 1;
    }

    fixture fx = {0};
    fx.context = ctx;
    fx.extent = (VkExtent2D){ w, h };

    char vspv[512], fspv[512], vert_post[512], frag_post[512], hist[512], avg[512];
    snprintf(vspv, sizeof vspv, "%s/pbr_forward.vert.spv", PEABERRY_SHADER_DIR);
    snprintf(fspv, sizeof fspv, "%s/pbr_forward.frag.spv", PEABERRY_SHADER_DIR);
    snprintf(vert_post, sizeof vert_post, "%s/fullscreen.vert.spv", PEABERRY_SHADER_DIR);
    snprintf(frag_post, sizeof frag_post, "%s/tonemap.frag.spv", PEABERRY_SHADER_DIR);
    snprintf(hist, sizeof hist, "%s/exposure_histogram.comp.spv", PEABERRY_SHADER_DIR);
    snprintf(avg, sizeof avg, "%s/exposure_average.comp.spv", PEABERRY_SHADER_DIR);

    if (!make_depth(&fx) ||
        !pb_rhi_texture_create_2d(ctx, w, h, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &fx.hdr_color) ||
        !pb_rhi_texture_create_2d(ctx, w, h, 1, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &fx.ldr_color) ||
        !make_render_passes(&fx) || !make_framebuffers(&fx)) {
        fprintf(stderr, "render target setup failed\n");
        return 1;
    }

    fx.scene = pb_gltf_scene_create(&(pb_gltf_scene_desc){ .context = ctx, .path = model });
    if (!fx.scene) { fprintf(stderr, "load failed: %s\n", model); return 1; }

    fx.pass = pb_pbr_forward_pass_create(&(pb_pbr_forward_pass_desc){
        .context = ctx, .render_pass = fx.hdr_render_pass,
        .vert_spv_path = vspv, .frag_spv_path = fspv,
        .ibl_shader_dir = PEABERRY_SHADER_DIR, .exposure = 1.0f });
    pb_pbr_forward_pass_set_scene(fx.pass, fx.scene);

    /* Match the espresso viewer's default orbit camera: azimuth 0,
     * elevation ~0.4 rad, distance 3, target origin, 45deg FOV. The default
     * forward-pass camera (eye 0,0,3 — straight on) shows the cube face-on
     * and hides the top face, so artifacts on the top face aren't visible.
     * Set DOWNTILT=1 in the env to render from a near-top-down view (reproduces
     * the "background turns white when looking down" auto-exposure failure). */
    {
        const float az = 0.0f;
        const char *tilt = getenv("PB_CAPTURE_ELEVATION");
        const float el = tilt ? atof(tilt) : (getenv("PB_CAPTURE_DOWNTILT") ? 1.45f : 0.4f);
        const float dist = 3.0f;
        const float eye[3] = {
            dist * cosf(el) * sinf(az),
            dist * sinf(el),
            dist * cosf(el) * cosf(az),
        };
        pb_mat4 view, proj;
        pb_mat4_look_at(view, eye, (float[]){0,0,0}, (float[]){0,1,0});
        pb_mat4_perspective(proj, pb_radians(45.0f),
            (float)w / (float)h, 0.1f, 100.0f);
        pb_pbr_forward_pass_set_camera(fx.pass, view, proj, eye);
    }

    /* Match the viewer's default directional light — but defer to lights the
     * glTF scene defines via KHR_lights_punctual (Phase 13.3). */
    if (pb_gltf_scene_light_count(fx.scene) == 0) {
        pb_light light = {0};
        light.type = PB_LIGHT_TYPE_DIRECTIONAL;
        light.direction[0] = 0.5f; light.direction[1] = 0.8f; light.direction[2] = 0.4f;
        light.color[0] = 4.0f; light.color[1] = 4.0f; light.color[2] = 4.0f;
        light.shadow_map_index = UINT32_MAX;
        pb_pbr_forward_pass_set_lights(fx.pass, &light, 1);
    }
    pb_pbr_forward_pass_set_shadows_enabled(fx.pass, use_shadows);
    pb_pbr_forward_pass_set_frustum_culling_enabled(fx.pass, false);
    pb_pbr_forward_pass_set_ibl_intensity(fx.pass, getenv("PB_IBL") ? atof(getenv("PB_IBL")) : 0.3f);
    pb_pbr_forward_pass_set_shadow_debug(fx.pass, use_shadow_debug || use_depth_viz);
    (void)use_depth_viz;

    if (use_auto_exposure) {
        fx.exposure = pb_pbr_exposure_pass_create(&(pb_pbr_exposure_pass_desc){
            .context = ctx, .histogram_spv_path = hist, .average_spv_path = avg, .initial_exposure = 1.0f });
    }
    fx.post = pb_pbr_post_pass_create(&(pb_pbr_post_pass_desc){
        .context = ctx, .render_pass = fx.ldr_render_pass,
        .vert_spv_path = vert_post, .frag_spv_path = frag_post,
        .exposure = 1.0f, .exposure_pass = fx.exposure });

    /* Run several frames so the auto-exposure converges to a stable value. */
    for (int i = 0; i < 60; ++i) {
        pb_rhi_submit_one_shot(ctx, record_frame, &fx);
    }

    /* Read back the LDR target and write the PNG. */
    VkDeviceSize sz = (VkDeviceSize)w * h * 4;
    pb_rhi_buffer staging = {0};
    pb_rhi_buffer_desc sd = { .size = sz, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU };
    if (!pb_rhi_buffer_create(ctx, &sd, &staging)) {
        fprintf(stderr, "staging alloc failed\n");
        return 1;
    }
    readback_ctx rbc = { &fx, &staging };
    pb_rhi_submit_one_shot(ctx, record_readback, &rbc);

    void *mapped = NULL;
    if (vkMapMemory(pb_context_device(ctx), staging.memory, 0, sz, 0, &mapped) != VK_SUCCESS) {
        fprintf(stderr, "map failed\n");
        return 1;
    }
    stbi_write_png(out, (int)w, (int)h, 4, mapped, (int)w * 4);
    vkUnmapMemory(pb_context_device(ctx), staging.memory);
    pb_rhi_buffer_destroy(ctx, &staging);

    printf("wrote %s (%ux%u), adapted exposure = %.4f\n",
        out, w, h, pb_pbr_exposure_pass_current(fx.exposure));
    return 0;
}
