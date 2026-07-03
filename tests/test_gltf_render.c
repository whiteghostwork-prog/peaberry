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
    pb_gltf_scene *scene;
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    pb_rhi_texture color;
    VkImage depth_image;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;
    VkFormat depth_format;
    VkExtent2D extent;
} gltf_render_fixture;

typedef struct gltf_render_record_ctx {
    gltf_render_fixture *fixture;
    float time_seconds;
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

static bool create_render_pass(gltf_render_fixture *fx)
{
    VkAttachmentDescription attachments[2] = {
        {
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
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

    return vkCreateRenderPass(pb_context_device(fx->context), &render_pass_info, NULL, &fx->render_pass) ==
        VK_SUCCESS;
}

static bool create_framebuffer(gltf_render_fixture *fx)
{
    VkImageView attachments[] = { fx->color.view, fx->depth_view };
    VkFramebufferCreateInfo fb_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = fx->render_pass,
        .attachmentCount = 2,
        .pAttachments = attachments,
        .width = fx->extent.width,
        .height = fx->extent.height,
        .layers = 1,
    };

    return vkCreateFramebuffer(pb_context_device(fx->context), &fb_info, NULL, &fx->framebuffer) == VK_SUCCESS;
}

static void destroy_fixture(gltf_render_fixture *fx)
{
    if (!fx || !fx->context || !pb_context_device_ready(fx->context)) {
        return;
    }

    pb_context_wait_device_idle(fx->context);
    VkDevice device = pb_context_device(fx->context);

    if (fx->pass) {
        pb_pbr_forward_pass_destroy(fx->pass);
        fx->pass = NULL;
    }
    if (fx->scene) {
        pb_gltf_scene_destroy(fx->scene);
        fx->scene = NULL;
    }
    if (fx->framebuffer) {
        vkDestroyFramebuffer(device, fx->framebuffer, NULL);
        fx->framebuffer = VK_NULL_HANDLE;
    }
    if (fx->render_pass) {
        vkDestroyRenderPass(device, fx->render_pass, NULL);
        fx->render_pass = VK_NULL_HANDLE;
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
    pb_rhi_texture_destroy(fx->context, &fx->color);
}

static void record_gltf_frame(VkCommandBuffer cmd, void *user_data)
{
    gltf_render_record_ctx *ctx = user_data;
    gltf_render_fixture *fx = ctx->fixture;

    pb_pbr_forward_pass_record_shadow_map(fx->pass, cmd, fx->extent, fx->scene);

    VkClearValue clears[2] = {
        { .color = { { 0.02f, 0.02f, 0.025f, 1.0f } } },
        { .depthStencil = { 1.0f, 0 } },
    };

    VkRenderPassBeginInfo rp_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = fx->render_pass,
        .framebuffer = fx->framebuffer,
        .renderArea = { .extent = fx->extent },
        .clearValueCount = 2,
        .pClearValues = clears,
    };

    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    pb_pbr_forward_pass_record(fx->pass, cmd, fx->extent, fx->scene, ctx->time_seconds);
    vkCmdEndRenderPass(cmd);
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
        fx->color.image,
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
        .renderPass = fx->render_pass,
        .framebuffer = fx->framebuffer,
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
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            &fx.color) ||
        !create_render_pass(&fx) ||
        !create_framebuffer(&fx)) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("failed to create headless render target");
    }

    pb_sphere_pass *sphere = pb_sphere_pass_create(
        &(pb_sphere_pass_desc){
            .context = ctx,
            .render_pass = fx.render_pass,
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
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            &fx.color) ||
        !create_render_pass(&fx) ||
        !create_framebuffer(&fx)) {
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
            .render_pass = fx.render_pass,
            .vert_spv_path = vert_spv,
            .frag_spv_path = frag_spv,
            .ibl_shader_dir = PEABERRY_SHADER_DIR,
            .exposure = 1.2f,
        });
    PB_ASSERT(fx.pass != NULL);
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

    VkClearValue clears[2] = {
        { .color = { { 0.02f, 0.02f, 0.025f, 1.0f } } },
        { .depthStencil = { 1.0f, 0 } },
    };

    VkRenderPassBeginInfo rp_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = fx->render_pass,
        .framebuffer = fx->framebuffer,
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
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            &fx.color) ||
        !create_render_pass(&fx) ||
        !create_framebuffer(&fx)) {
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
            .render_pass = fx.render_pass,
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

#ifdef PEABERRY_ENABLE_RAYTRACING
PB_TEST(test_gltf_rt_hybrid_render_smoke)
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
            .app_name = "peaberry rt hybrid render test",
            .enable_validation = false,
            .enable_surface = false,
        });
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    if (!pb_context_raytracing_supported(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("ray tracing unsupported on this GPU");
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
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            &fx.color) ||
        !create_render_pass(&fx) ||
        !create_framebuffer(&fx)) {
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
            .render_pass = fx.render_pass,
            .vert_spv_path = vert_spv,
            .frag_spv_path = frag_spv,
            .ibl_shader_dir = PEABERRY_SHADER_DIR,
            .exposure = 1.2f,
        });
    if (!fx.pass) {
        destroy_fixture(&fx);
        pb_context_destroy(ctx);
        PB_TEST_SKIP("forward pass create failed");
    }

    pb_pbr_forward_pass_set_scene(fx.pass, fx.scene);
    PB_ASSERT(pb_pbr_forward_pass_scene_is_bound(fx.pass));
    PB_ASSERT(pb_pbr_forward_pass_raytracing_available(fx.pass));
    pb_pbr_forward_pass_set_raytracing_enabled(fx.pass, true);

    gltf_render_record_ctx record_ctx = { .fixture = &fx, .time_seconds = 0.0f };
    PB_ASSERT(pb_rhi_submit_one_shot(ctx, record_gltf_frame, &record_ctx));

    uint8_t rgba[4] = {0};
    PB_ASSERT(read_center_pixel(&fx, rgba));

    const bool not_clear =
        rgba[0] > 8 || rgba[1] > 8 || rgba[2] > 8 || rgba[0] != 5 || rgba[1] != 5 || rgba[2] != 6;
    PB_ASSERT(not_clear);

    destroy_fixture(&fx);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}
#endif

void pb_run_gltf_render_tests(void)
{
    printf("gltf render tests\n");
    PB_RUN_TEST(test_sphere_forward_pass_pixel);
    PB_RUN_TEST(test_gltf_forward_pass_pixel);
    PB_RUN_TEST(test_gltf_draw_with_sphere_pass);
#ifdef PEABERRY_ENABLE_RAYTRACING
    PB_RUN_TEST(test_gltf_rt_hybrid_render_smoke);
#endif
}
