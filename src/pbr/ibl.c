/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pbr/ibl.h"

#include "core/log.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/cmd_submit.h"
#include "rhi/cubemap.h"
#include "rhi/offscreen.h"
#include "rhi/shader.h"
#include "rhi/texture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PB_BRDF_LUT_SIZE = 512,
    PB_ENV_CUBE_SIZE = 512,
    PB_IRRADIANCE_SIZE = 32,
    PB_PREFILTER_SIZE = 128,
    PB_PREFILTER_MIPS = 5,
};

typedef struct pb_ibl_push {
    int face;
    float roughness;
    float target_size[2];
} pb_ibl_push;

enum { PB_IBL_MAX_DEFERRED = 64 };

typedef struct pb_ibl_deferred {
    VkFramebuffer framebuffers[PB_IBL_MAX_DEFERRED];
    size_t framebuffer_count;
    VkImageView face_views[PB_IBL_MAX_DEFERRED];
    size_t face_view_count;
} pb_ibl_deferred;

typedef struct pb_ibl_bake {
    pb_context *context;
    pb_rhi_texture equirect;
    pb_rhi_cubemap env_cube;
    pb_ibl_environment *env;
    pb_ibl_deferred deferred;
    VkRenderPass cube_render_pass;
    VkRenderPass brdf_render_pass;
    VkShaderModule fullscreen_vert;
    VkShaderModule equirect_frag;
    VkShaderModule irradiance_frag;
    VkShaderModule prefilter_frag;
    VkShaderModule brdf_frag;
    VkPipelineLayout equirect_layout;
    VkPipelineLayout irradiance_layout;
    VkPipelineLayout prefilter_layout;
    VkPipelineLayout brdf_layout;
    VkPipeline equirect_pipeline;
    VkPipeline irradiance_pipeline;
    VkPipeline prefilter_pipeline;
    VkPipeline brdf_pipeline;
    VkDescriptorSetLayout equirect_set_layout;
    VkDescriptorSetLayout irradiance_set_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet equirect_set;
    VkDescriptorSet irradiance_set;
    VkDescriptorSet prefilter_set;
} pb_ibl_bake;

static bool join_shader_path(
    const char *shader_dir,
    const char *name,
    char *out,
    size_t out_size)
{
    const int written = snprintf(out, out_size, "%s/%s", shader_dir, name);
    return written > 0 && (size_t)written < out_size;
}

static float *create_procedural_equirect(uint32_t *out_width, uint32_t *out_height)
{
    const uint32_t width = 512;
    const uint32_t height = 256;
    float *pixels = calloc((size_t)width * (size_t)height, 4 * sizeof(float));
    if (!pixels) {
        return NULL;
    }

    for (uint32_t y = 0; y < height; ++y) {
        const float v = (float)y / (float)(height - 1);
        const float zenith = 1.0f - v;
        for (uint32_t x = 0; x < width; ++x) {
            const float u = (float)x / (float)(width - 1);
            const float sun = fmaxf(0.0f, sinf(u * 6.2831853f) * 0.15f + 0.85f);
            float *pixel = pixels + ((size_t)y * width + x) * 4;
            pixel[0] = (0.15f + 0.55f * zenith) * sun;
            pixel[1] = (0.20f + 0.45f * zenith) * sun;
            pixel[2] = (0.35f + 0.40f * zenith) * sun;
            pixel[3] = 1.0f;
        }
    }

    *out_width = width;
    *out_height = height;
    return pixels;
}

static void deferred_destroy_all(pb_context *context, pb_ibl_deferred *deferred)
{
    if (!context || !deferred) {
        return;
    }

    VkDevice device = pb_context_device(context);

    for (size_t i = 0; i < deferred->framebuffer_count; ++i) {
        pb_rhi_offscreen_framebuffer_destroy(context, deferred->framebuffers[i]);
    }
    for (size_t i = 0; i < deferred->face_view_count; ++i) {
        vkDestroyImageView(device, deferred->face_views[i], NULL);
    }

    *deferred = (pb_ibl_deferred){0};
}

static bool deferred_push_framebuffer(pb_ibl_deferred *deferred, VkFramebuffer framebuffer)
{
    if (!deferred || framebuffer == VK_NULL_HANDLE || deferred->framebuffer_count >= PB_IBL_MAX_DEFERRED) {
        return false;
    }

    deferred->framebuffers[deferred->framebuffer_count++] = framebuffer;
    return true;
}

static bool deferred_push_face_view(pb_ibl_deferred *deferred, VkImageView face_view)
{
    if (!deferred || face_view == VK_NULL_HANDLE || deferred->face_view_count >= PB_IBL_MAX_DEFERRED) {
        return false;
    }

    deferred->face_views[deferred->face_view_count++] = face_view;
    return true;
}

static bool create_fullscreen_pipeline(
    pb_context *context,
    VkRenderPass render_pass,
    VkShaderModule vert,
    VkShaderModule frag,
    VkDescriptorSetLayout set_layout,
    VkPipelineLayout *out_layout,
    VkPipeline *out_pipeline,
    size_t push_size)
{
    VkDevice device = pb_context_device(context);

    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = set_layout ? 1 : 0,
        .pSetLayouts = set_layout ? &set_layout : NULL,
    };

    if (push_size > 0) {
        VkPushConstantRange push = {
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = (uint32_t)push_size,
        };
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
    }

    if (vkCreatePipelineLayout(device, &layout_info, NULL, out_layout) != VK_SUCCESS) {
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag,
            .pName = "main",
        },
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };

    VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pColorBlendState = &color_blend,
        .pDynamicState = &dynamic_state,
        .layout = *out_layout,
        .renderPass = render_pass,
        .subpass = 0,
    };

    return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, out_pipeline) ==
           VK_SUCCESS;
}

static void destroy_bake_pipelines(pb_context *context, pb_ibl_bake *bake)
{
    VkDevice device = pb_context_device(context);

    if (bake->brdf_render_pass) {
        pb_rhi_offscreen_render_pass_destroy(context, bake->brdf_render_pass);
    }
    if (bake->cube_render_pass) {
        pb_rhi_offscreen_render_pass_destroy(context, bake->cube_render_pass);
    }
    if (bake->brdf_pipeline) {
        vkDestroyPipeline(device, bake->brdf_pipeline, NULL);
    }
    if (bake->prefilter_pipeline) {
        vkDestroyPipeline(device, bake->prefilter_pipeline, NULL);
    }
    if (bake->irradiance_pipeline) {
        vkDestroyPipeline(device, bake->irradiance_pipeline, NULL);
    }
    if (bake->equirect_pipeline) {
        vkDestroyPipeline(device, bake->equirect_pipeline, NULL);
    }
    if (bake->brdf_layout) {
        vkDestroyPipelineLayout(device, bake->brdf_layout, NULL);
    }
    if (bake->prefilter_layout) {
        vkDestroyPipelineLayout(device, bake->prefilter_layout, NULL);
    }
    if (bake->irradiance_layout) {
        vkDestroyPipelineLayout(device, bake->irradiance_layout, NULL);
    }
    if (bake->equirect_layout) {
        vkDestroyPipelineLayout(device, bake->equirect_layout, NULL);
    }
    if (bake->descriptor_pool) {
        vkDestroyDescriptorPool(device, bake->descriptor_pool, NULL);
    }
    if (bake->irradiance_set_layout) {
        vkDestroyDescriptorSetLayout(device, bake->irradiance_set_layout, NULL);
    }
    if (bake->equirect_set_layout) {
        vkDestroyDescriptorSetLayout(device, bake->equirect_set_layout, NULL);
    }
    if (bake->brdf_frag) {
        vkDestroyShaderModule(device, bake->brdf_frag, NULL);
    }
    if (bake->prefilter_frag) {
        vkDestroyShaderModule(device, bake->prefilter_frag, NULL);
    }
    if (bake->irradiance_frag) {
        vkDestroyShaderModule(device, bake->irradiance_frag, NULL);
    }
    if (bake->equirect_frag) {
        vkDestroyShaderModule(device, bake->equirect_frag, NULL);
    }
    if (bake->fullscreen_vert) {
        vkDestroyShaderModule(device, bake->fullscreen_vert, NULL);
    }
}

static void transition_cubemap_mip(
    VkCommandBuffer cmd,
    pb_rhi_cubemap *cubemap,
    uint32_t mip_level,
    VkImageLayout old_layout,
    VkImageLayout new_layout)
{
    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkAccessFlags src_access = 0;
    VkAccessFlags dst_access = VK_ACCESS_SHADER_READ_BIT;

    if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        src_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if (new_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dst_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .image = cubemap->image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = mip_level,
            .levelCount = 1,
            .layerCount = 6,
        },
    };

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void render_cubemap_pass(
    VkCommandBuffer cmd,
    pb_ibl_bake *bake,
    pb_rhi_cubemap *target,
    uint32_t mip_level,
    VkPipeline pipeline,
    VkPipelineLayout layout,
    VkDescriptorSet descriptor_set,
    float roughness)
{
    const uint32_t size = target->size >> mip_level;
    const VkExtent2D extent = { size, size };

    transition_cubemap_mip(
        cmd,
        target,
        mip_level,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    for (int face = 0; face < 6; ++face) {
        VkImageView face_view = VK_NULL_HANDLE;
        if (!pb_rhi_cubemap_create_face_view(bake->context, target, (uint32_t)face, mip_level, &face_view)) {
            continue;
        }

        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        if (!pb_rhi_offscreen_framebuffer_create(
                bake->context,
                bake->cube_render_pass,
                face_view,
                extent,
                &framebuffer)) {
            vkDestroyImageView(pb_context_device(bake->context), face_view, NULL);
            continue;
        }

        deferred_push_framebuffer(&bake->deferred, framebuffer);
        deferred_push_face_view(&bake->deferred, face_view);

        VkClearValue clear = { .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } };
        VkRenderPassBeginInfo begin_info = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = bake->cube_render_pass,
            .framebuffer = framebuffer,
            .renderArea = { .extent = extent },
            .clearValueCount = 1,
            .pClearValues = &clear,
        };

        vkCmdBeginRenderPass(cmd, &begin_info, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport = {
            .width = (float)extent.width,
            .height = (float)extent.height,
            .maxDepth = 1.0f,
        };
        VkRect2D scissor = { .extent = extent };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        pb_ibl_push push = {
            .face = face,
            .roughness = roughness,
            .target_size = { (float)extent.width, (float)extent.height },
        };

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        if (descriptor_set) {
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                layout,
                0,
                1,
                &descriptor_set,
                0,
                NULL);
        }
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    transition_cubemap_mip(
        cmd,
        target,
        mip_level,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static void record_ibl_bake(VkCommandBuffer cmd, void *user_data)
{
    pb_ibl_bake *bake = user_data;

    render_cubemap_pass(
        cmd,
        bake,
        &bake->env_cube,
        0,
        bake->equirect_pipeline,
        bake->equirect_layout,
        bake->equirect_set,
        0.0f);

    render_cubemap_pass(
        cmd,
        bake,
        &bake->env->irradiance,
        0,
        bake->irradiance_pipeline,
        bake->irradiance_layout,
        bake->irradiance_set,
        0.0f);

    for (uint32_t mip = 0; mip < bake->env->prefilter.mip_levels; ++mip) {
        const float roughness =
            bake->env->prefilter.mip_levels <= 1
                ? 0.0f
                : (float)mip / (float)(bake->env->prefilter.mip_levels - 1);
        render_cubemap_pass(
            cmd,
            bake,
            &bake->env->prefilter,
            mip,
            bake->prefilter_pipeline,
            bake->prefilter_layout,
            bake->prefilter_set,
            roughness);
    }

    pb_rhi_texture_transition_layout(
        cmd,
        &bake->env->brdf_lut,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        1,
        1);

    const VkExtent2D brdf_extent = { PB_BRDF_LUT_SIZE, PB_BRDF_LUT_SIZE };
    VkFramebuffer brdf_framebuffer = VK_NULL_HANDLE;
    if (pb_rhi_offscreen_framebuffer_create(
            bake->context,
            bake->brdf_render_pass,
            bake->env->brdf_lut.view,
            brdf_extent,
            &brdf_framebuffer)) {
        deferred_push_framebuffer(&bake->deferred, brdf_framebuffer);

        VkClearValue clear = { .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } };
        VkRenderPassBeginInfo begin_info = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = bake->brdf_render_pass,
            .framebuffer = brdf_framebuffer,
            .renderArea = { .extent = brdf_extent },
            .clearValueCount = 1,
            .pClearValues = &clear,
        };

        vkCmdBeginRenderPass(cmd, &begin_info, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport = {
            .width = (float)PB_BRDF_LUT_SIZE,
            .height = (float)PB_BRDF_LUT_SIZE,
            .maxDepth = 1.0f,
        };
        VkRect2D scissor = { .extent = brdf_extent };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bake->brdf_pipeline);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    pb_rhi_texture_transition_layout(
        cmd,
        &bake->env->brdf_lut,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        1,
        1);
}

bool pb_ibl_environment_create(const pb_ibl_environment_desc *desc, pb_ibl_environment *out_env)
{
    if (!desc || !desc->context || !desc->shader_dir || !out_env) {
        return false;
    }

    *out_env = (pb_ibl_environment){0};
    out_env->prefilter_max_lod = (float)(PB_PREFILTER_MIPS - 1);

    char shader_path[512];

    pb_ibl_bake bake = { .context = desc->context, .env = out_env };
    VkDevice device = pb_context_device(desc->context);

    if (!join_shader_path(desc->shader_dir, "fullscreen.vert.spv", shader_path, sizeof(shader_path)) ||
        !pb_rhi_shader_module_from_file(device, shader_path, &bake.fullscreen_vert)) {
        return false;
    }

    if (!join_shader_path(desc->shader_dir, "equirect_to_cubemap.frag.spv", shader_path, sizeof(shader_path)) ||
        !pb_rhi_shader_module_from_file(device, shader_path, &bake.equirect_frag)) {
        destroy_bake_pipelines(desc->context, &bake);
        return false;
    }

    if (!join_shader_path(desc->shader_dir, "irradiance.frag.spv", shader_path, sizeof(shader_path)) ||
        !pb_rhi_shader_module_from_file(device, shader_path, &bake.irradiance_frag)) {
        destroy_bake_pipelines(desc->context, &bake);
        return false;
    }

    if (!join_shader_path(desc->shader_dir, "prefilter.frag.spv", shader_path, sizeof(shader_path)) ||
        !pb_rhi_shader_module_from_file(device, shader_path, &bake.prefilter_frag)) {
        destroy_bake_pipelines(desc->context, &bake);
        return false;
    }

    if (!join_shader_path(desc->shader_dir, "brdf_lut.frag.spv", shader_path, sizeof(shader_path)) ||
        !pb_rhi_shader_module_from_file(device, shader_path, &bake.brdf_frag)) {
        destroy_bake_pipelines(desc->context, &bake);
        return false;
    }

    if (desc->equirect_hdr_path) {
        if (!pb_rhi_texture_create_from_hdr_file(desc->context, desc->equirect_hdr_path, &bake.equirect)) {
            destroy_bake_pipelines(desc->context, &bake);
            return false;
        }
    } else {
        uint32_t width = 0;
        uint32_t height = 0;
        float *pixels = create_procedural_equirect(&width, &height);
        if (!pixels ||
            !pb_rhi_texture_create_2d(
                desc->context,
                width,
                height,
                1,
                VK_FORMAT_R32G32B32A32_SFLOAT,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                &bake.equirect) ||
            !pb_rhi_texture_upload_rgba32f(desc->context, &bake.equirect, pixels, width, height)) {
            free(pixels);
            destroy_bake_pipelines(desc->context, &bake);
            return false;
        }
        free(pixels);
    }

    const VkFormat cube_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkImageUsageFlags cube_usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    if (!pb_rhi_cubemap_create(
            desc->context,
            PB_ENV_CUBE_SIZE,
            1,
            cube_format,
            cube_usage,
            &bake.env_cube) ||
        !pb_rhi_cubemap_create(
            desc->context,
            PB_IRRADIANCE_SIZE,
            1,
            cube_format,
            cube_usage,
            &out_env->irradiance) ||
        !pb_rhi_cubemap_create(
            desc->context,
            PB_PREFILTER_SIZE,
            PB_PREFILTER_MIPS,
            cube_format,
            cube_usage,
            &out_env->prefilter) ||
        !pb_rhi_texture_create_2d(
            desc->context,
            PB_BRDF_LUT_SIZE,
            PB_BRDF_LUT_SIZE,
            1,
            VK_FORMAT_R16G16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            &out_env->brdf_lut)) {
        pb_rhi_texture_destroy(desc->context, &bake.equirect);
        pb_rhi_cubemap_destroy(desc->context, &bake.env_cube);
        pb_ibl_environment_destroy(desc->context, out_env);
        destroy_bake_pipelines(desc->context, &bake);
        return false;
    }

    VkDescriptorSetLayoutBinding sampler_binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };

    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &sampler_binding,
    };

    if (vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &bake.equirect_set_layout) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &bake.irradiance_set_layout) != VK_SUCCESS) {
        pb_ibl_environment_destroy(desc->context, out_env);
        pb_rhi_texture_destroy(desc->context, &bake.equirect);
        pb_rhi_cubemap_destroy(desc->context, &bake.env_cube);
        destroy_bake_pipelines(desc->context, &bake);
        return false;
    }

    if (!pb_rhi_offscreen_render_pass_create(desc->context, cube_format, &bake.cube_render_pass) ||
        !pb_rhi_offscreen_render_pass_create(
            desc->context,
            VK_FORMAT_R16G16_SFLOAT,
            &bake.brdf_render_pass)) {
        pb_ibl_environment_destroy(desc->context, out_env);
        pb_rhi_texture_destroy(desc->context, &bake.equirect);
        pb_rhi_cubemap_destroy(desc->context, &bake.env_cube);
        destroy_bake_pipelines(desc->context, &bake);
        return false;
    }

    if (!create_fullscreen_pipeline(
            desc->context,
            bake.cube_render_pass,
            bake.fullscreen_vert,
            bake.equirect_frag,
            bake.equirect_set_layout,
            &bake.equirect_layout,
            &bake.equirect_pipeline,
            sizeof(pb_ibl_push)) ||
        !create_fullscreen_pipeline(
            desc->context,
            bake.cube_render_pass,
            bake.fullscreen_vert,
            bake.irradiance_frag,
            bake.irradiance_set_layout,
            &bake.irradiance_layout,
            &bake.irradiance_pipeline,
            sizeof(pb_ibl_push)) ||
        !create_fullscreen_pipeline(
            desc->context,
            bake.cube_render_pass,
            bake.fullscreen_vert,
            bake.prefilter_frag,
            bake.irradiance_set_layout,
            &bake.prefilter_layout,
            &bake.prefilter_pipeline,
            sizeof(pb_ibl_push)) ||
        !create_fullscreen_pipeline(
            desc->context,
            bake.brdf_render_pass,
            bake.fullscreen_vert,
            bake.brdf_frag,
            VK_NULL_HANDLE,
            &bake.brdf_layout,
            &bake.brdf_pipeline,
            0)) {
        pb_ibl_environment_destroy(desc->context, out_env);
        pb_rhi_texture_destroy(desc->context, &bake.equirect);
        pb_rhi_cubemap_destroy(desc->context, &bake.env_cube);
        destroy_bake_pipelines(desc->context, &bake);
        return false;
    }

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 3,
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 3,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };

    if (vkCreateDescriptorPool(device, &pool_info, NULL, &bake.descriptor_pool) != VK_SUCCESS) {
        pb_ibl_environment_destroy(desc->context, out_env);
        pb_rhi_texture_destroy(desc->context, &bake.equirect);
        pb_rhi_cubemap_destroy(desc->context, &bake.env_cube);
        destroy_bake_pipelines(desc->context, &bake);
        return false;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = bake.descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &bake.equirect_set_layout,
    };

    vkAllocateDescriptorSets(device, &alloc_info, &bake.equirect_set);
    alloc_info.pSetLayouts = &bake.irradiance_set_layout;
    vkAllocateDescriptorSets(device, &alloc_info, &bake.irradiance_set);
    vkAllocateDescriptorSets(device, &alloc_info, &bake.prefilter_set);

    VkDescriptorImageInfo equirect_info = {
        .sampler = bake.equirect.sampler,
        .imageView = bake.equirect.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkWriteDescriptorSet write_equirect = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = bake.equirect_set,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &equirect_info,
    };

    vkUpdateDescriptorSets(device, 1, &write_equirect, 0, NULL);

    VkDescriptorImageInfo env_info = {
        .sampler = bake.env_cube.sampler,
        .imageView = bake.env_cube.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkWriteDescriptorSet write_env = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = bake.irradiance_set,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &env_info,
    };

    vkUpdateDescriptorSets(device, 1, &write_env, 0, NULL);

    write_env.dstSet = bake.prefilter_set;
    vkUpdateDescriptorSets(device, 1, &write_env, 0, NULL);

    if (!pb_rhi_submit_one_shot(desc->context, record_ibl_bake, &bake)) {
        deferred_destroy_all(desc->context, &bake.deferred);
        pb_ibl_environment_destroy(desc->context, out_env);
        pb_rhi_texture_destroy(desc->context, &bake.equirect);
        pb_rhi_cubemap_destroy(desc->context, &bake.env_cube);
        destroy_bake_pipelines(desc->context, &bake);
        return false;
    }

    deferred_destroy_all(desc->context, &bake.deferred);

    pb_rhi_texture_destroy(desc->context, &bake.equirect);
    pb_rhi_cubemap_destroy(desc->context, &bake.env_cube);
    destroy_bake_pipelines(desc->context, &bake);

    pb_log_info("IBL environment maps ready");
    return true;
}

void pb_ibl_environment_destroy(pb_context *context, pb_ibl_environment *env)
{
    if (!context || !env) {
        return;
    }

    pb_rhi_texture_destroy(context, &env->brdf_lut);
    pb_rhi_cubemap_destroy(context, &env->irradiance);
    pb_rhi_cubemap_destroy(context, &env->prefilter);
    *env = (pb_ibl_environment){0};
}
