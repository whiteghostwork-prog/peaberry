/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bloom_pass.h"
#include "core/log.h"
#include "exposure_pass.h"
#include "pb_context_internal.h"
#include "peaberry/peaberry_gltf.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/buffer.h"
#include "rhi/shader.h"
#include "rhi/texture.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* std140 vec4 minimum for a uniform block. tonemap.frag only reads the .x
 * (exposure float); the rest is padding. */
#define PB_POST_EXPOSURE_UBO_BYTES 16u

typedef struct pb_pbr_post_pass {
    pb_context *context;
    VkPipeline pipeline;
    VkPipelineLayout pipeline_layout;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkShaderModule vert_module;
    VkShaderModule frag_module;

    /* Exposure UBO source. If exposure_pass is set, the post pass binds the
     * pass's adapted UBO. Otherwise it owns a small static UBO seeded with the
     * fixed `exposure` value from the desc (Phase 15.1 backward-compat). */
    pb_pbr_exposure_pass *exposure_pass;
    pb_rhi_buffer static_exposure_ubo;

    /* Bloom result (Phase 15.3). If bloom_pass is set, binding 2 points at its
     * result texture; otherwise a 1x1 black texture is bound so tonemap adds
     * zero bloom (backward compat). bloom_intensity is pushed each frame. */
    pb_pbr_bloom_pass *bloom_pass;
    pb_rhi_texture black_fallback;
    float bloom_intensity;

    /* Cached (view, sampler) currently bound to descriptor_set. The HDR scene
     * view is usually the same every frame, so we skip vkUpdateDescriptorSets
     * when unchanged — this also avoids the "descriptor set in use" race that
     * would otherwise occur when re-writing a set referenced by a previous
     * frame's still-pending command buffer. */
    VkImageView bound_view;
    VkSampler bound_sampler;
    VkImageView bound_bloom_view;
    bool descriptor_written;
} pb_pbr_post_pass;

static bool create_static_exposure_ubo(pb_context *context, float exposure, pb_rhi_buffer *out)
{
    /* SSBO, not a UBO: the tonemap shader declares exposure as a readonly
     * storage buffer so the same binding works whether the buffer is the
     * exposure pass's GPU-written SSBO or this static fallback. */
    pb_rhi_buffer_desc desc = {
        .size = PB_POST_EXPOSURE_UBO_BYTES,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };
    if (!pb_rhi_buffer_create(context, &desc, out)) {
        return false;
    }
    float padded[4] = { exposure, 0.0f, 0.0f, 0.0f };
    memcpy(out->mapped, padded, sizeof(padded));
    return true;
}

static bool create_descriptor_set_layout(pb_context *context, VkDescriptorSetLayout *out_layout)
{
    VkDescriptorSetLayoutBinding bindings[] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = sizeof(bindings) / sizeof(bindings[0]),
        .pBindings = bindings,
    };

    return vkCreateDescriptorSetLayout(pb_context_device(context), &layout_info, NULL, out_layout) ==
           VK_SUCCESS;
}

static bool create_pipeline(
    pb_context *context,
    VkRenderPass render_pass,
    VkShaderModule vert,
    VkShaderModule frag,
    VkDescriptorSetLayout set_layout,
    VkPipelineLayout *out_layout,
    VkPipeline *out_pipeline)
{
    VkDevice device = pb_context_device(context);

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(float),   /* bloom_intensity */
    };

    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };

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

    /* The tonemap pass doesn't touch depth, but the caller's LDR render pass
     * may still declare a depth attachment (the test fixture does). Provide a
     * no-op depth/stencil state so the pipeline is compatible with such a
     * render pass — depth test disabled, no writes. */
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_FALSE,
        .depthWriteEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
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
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &color_blend,
        .pDynamicState = &dynamic_state,
        .layout = *out_layout,
        .renderPass = render_pass,
        .subpass = 0,
    };

    return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, out_pipeline) ==
           VK_SUCCESS;
}

static bool allocate_descriptor_set(pb_context *context, VkDescriptorSetLayout layout, VkDescriptorPool *out_pool, VkDescriptorSet *out_set)
{
    VkDescriptorPoolSize pool_sizes[] = {
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 2 },
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 },
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]),
        .pPoolSizes = pool_sizes,
    };

    if (vkCreateDescriptorPool(pb_context_device(context), &pool_info, NULL, out_pool) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = *out_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    };

    return vkAllocateDescriptorSets(pb_context_device(context), &alloc_info, out_set) == VK_SUCCESS;
}

pb_pbr_post_pass *pb_pbr_post_pass_create(const pb_pbr_post_pass_desc *desc)
{
    if (!desc || !desc->context || !desc->render_pass || !desc->vert_spv_path || !desc->frag_spv_path) {
        pb_log_error("pb_pbr_post_pass_create: missing required desc fields");
        return NULL;
    }

    pb_pbr_post_pass *pass = calloc(1, sizeof(*pass));
    if (!pass) {
        return NULL;
    }
    pass->context = desc->context;
    pass->exposure_pass = desc->exposure_pass;
    pass->bloom_pass = desc->bloom_pass;
    pass->bloom_intensity = desc->bloom_pass ? desc->bloom_intensity : 0.0f;

    /* If no auto-exposure pass is wired in, seed a static UBO with the
     * requested fixed exposure. */
    if (!pass->exposure_pass) {
        if (!create_static_exposure_ubo(desc->context, desc->exposure, &pass->static_exposure_ubo)) {
            pb_pbr_post_pass_destroy(pass);
            return NULL;
        }
    }

    VkDevice device = pb_context_device(desc->context);

    if (!pb_rhi_shader_module_from_file(device, desc->vert_spv_path, &pass->vert_module) ||
        !pb_rhi_shader_module_from_file(device, desc->frag_spv_path, &pass->frag_module) ||
        !create_descriptor_set_layout(desc->context, &pass->descriptor_set_layout) ||
        !allocate_descriptor_set(desc->context, pass->descriptor_set_layout, &pass->descriptor_pool, &pass->descriptor_set) ||
        !create_pipeline(
            desc->context,
            desc->render_pass,
            pass->vert_module,
            pass->frag_module,
            pass->descriptor_set_layout,
            &pass->pipeline_layout,
            &pass->pipeline)) {
        pb_pbr_post_pass_destroy(pass);
        return NULL;
    }

    /* Write binding 1 (exposure UBO) once at create time — the buffer handle
     * is stable for the pass's lifetime. Binding 0 (HDR view) is written in
     * record() with caching. */
    VkBuffer exposure_buffer = pass->exposure_pass
        ? pb_pbr_exposure_pass_ubo_handle(pass->exposure_pass)
        : pass->static_exposure_ubo.handle;

    VkDescriptorBufferInfo exposure_info = {
        .buffer = exposure_buffer,
        .offset = 0,
        .range = PB_POST_EXPOSURE_UBO_BYTES,
    };
    VkWriteDescriptorSet exposure_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = pass->descriptor_set,
        .dstBinding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .pBufferInfo = &exposure_info,
    };
    vkUpdateDescriptorSets(device, 1, &exposure_write, 0, NULL);

    /* Write binding 2 (bloom result). When no bloom pass is wired in, bind a
     * 1x1 black texture so tonemap adds zero bloom (backward compat). */
    if (!pass->bloom_pass) {
        if (!pb_rhi_texture_create_solid_rgba8(desc->context, (uint8_t[]){0, 0, 0, 255}, false, &pass->black_fallback)) {
            pb_pbr_post_pass_destroy(pass);
            return NULL;
        }
    }
    VkImageView bloom_view = pass->bloom_pass
        ? pb_pbr_bloom_pass_result_view(pass->bloom_pass)
        : pass->black_fallback.view;
    VkSampler bloom_sampler = pass->bloom_pass
        ? pb_pbr_bloom_pass_sampler(pass->bloom_pass)
        : pass->black_fallback.sampler;

    /* Only write binding 2 now if the view is valid. The bloom pass creates its
     * texture lazily on the first record(), so its result view may be NULL
     * here — in that case the record()-time rebinding fills it in. Writing a
     * NULL view would corrupt the descriptor set. */
    if (bloom_view != VK_NULL_HANDLE) {
        VkDescriptorImageInfo bloom_info = {
            .sampler = bloom_sampler,
            .imageView = bloom_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet bloom_write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = pass->descriptor_set,
            .dstBinding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &bloom_info,
        };
        vkUpdateDescriptorSets(device, 1, &bloom_write, 0, NULL);
        pass->bound_bloom_view = bloom_view;
    }

    return pass;
}

void pb_pbr_post_pass_destroy(pb_pbr_post_pass *pass)
{
    if (!pass) {
        return;
    }

    VkDevice device = pass->context ? pb_context_device(pass->context) : VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE && pb_context_device_ready(pass->context)) {
        if (pass->pipeline) {
            vkDestroyPipeline(device, pass->pipeline, NULL);
        }
        if (pass->pipeline_layout) {
            vkDestroyPipelineLayout(device, pass->pipeline_layout, NULL);
        }
        if (pass->descriptor_set_layout) {
            vkDestroyDescriptorSetLayout(device, pass->descriptor_set_layout, NULL);
        }
        if (pass->descriptor_pool) {
            vkDestroyDescriptorPool(device, pass->descriptor_pool, NULL);
        }
        if (pass->vert_module) {
            vkDestroyShaderModule(device, pass->vert_module, NULL);
        }
        if (pass->frag_module) {
            vkDestroyShaderModule(device, pass->frag_module, NULL);
        }
        if (pass->static_exposure_ubo.handle != VK_NULL_HANDLE) {
            pb_rhi_buffer_destroy(pass->context, &pass->static_exposure_ubo);
        }
        if (pass->black_fallback.image != VK_NULL_HANDLE) {
            pb_rhi_texture_destroy(pass->context, &pass->black_fallback);
        }
    }
    free(pass);
}

void pb_pbr_post_pass_record(
    pb_pbr_post_pass *pass,
    VkCommandBuffer cmd,
    VkExtent2D extent,
    VkImageView hdr_scene_view,
    VkSampler hdr_scene_sampler)
{
    if (!pass || !pass->pipeline) {
        return;
    }

    /* Update the descriptor set to point at this frame's HDR scene color. The
     * view + sampler are usually the same every frame (one HDR target), so we
     * skip vkUpdateDescriptorSets when unchanged. This also avoids re-writing
     * a descriptor set that a previous frame's still-pending command buffer
     * references (which the validator flags as a hazard). */
    if (!pass->descriptor_written ||
        pass->bound_view != hdr_scene_view ||
        pass->bound_sampler != hdr_scene_sampler) {
        VkDescriptorImageInfo image_info = {
            .sampler = hdr_scene_sampler,
            .imageView = hdr_scene_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = pass->descriptor_set,
            .dstBinding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &image_info,
        };

        vkUpdateDescriptorSets(pb_context_device(pass->context), 1, &write, 0, NULL);
        pass->bound_view = hdr_scene_view;
        pass->bound_sampler = hdr_scene_sampler;
        pass->descriptor_written = true;
    }

    /* Phase 15.3: rebind bloom (binding 2) when its result view changes. The
     * bloom pass creates its pyramid lazily on the first record() and may
     * resize it, so the view handle can differ from what was bound at create
     * time. Track the last bound bloom view to skip redundant writes. */
    if (pass->bloom_pass) {
        VkImageView bloom_view = pb_pbr_bloom_pass_result_view(pass->bloom_pass);
        if (bloom_view != pass->bound_bloom_view) {
            VkDescriptorImageInfo bloom_info = {
                .sampler = pb_pbr_bloom_pass_sampler(pass->bloom_pass),
                .imageView = bloom_view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
            VkWriteDescriptorSet write = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = pass->descriptor_set,
                .dstBinding = 2,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .pImageInfo = &bloom_info,
            };
            vkUpdateDescriptorSets(pb_context_device(pass->context), 1, &write, 0, NULL);
            pass->bound_bloom_view = bloom_view;
        }
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass->pipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pass->pipeline_layout,
        0,
        1,
        &pass->descriptor_set,
        0,
        NULL);

    const VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)extent.width,
        .height = (float)extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    const VkRect2D scissor = {
        .offset = {0, 0},
        .extent = extent,
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    /* Phase 15.3: push bloom_intensity (0 when no bloom pass is wired in). */
    vkCmdPushConstants(cmd, pass->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float), &pass->bloom_intensity);

    /* Fullscreen triangle generated by the vertex shader from gl_VertexIndex. */
    vkCmdDraw(cmd, 3, 1, 0, 0);
}
