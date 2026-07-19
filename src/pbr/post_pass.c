/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/log.h"
#include "pb_context_internal.h"
#include "peaberry/peaberry_gltf.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/shader.h"

#include <stddef.h>
#include <stdlib.h>

typedef struct pb_pbr_post_pass {
    pb_context *context;
    VkPipeline pipeline;
    VkPipelineLayout pipeline_layout;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkShaderModule vert_module;
    VkShaderModule frag_module;
    float exposure;
} pb_pbr_post_pass;

static bool create_descriptor_set_layout(pb_context *context, VkDescriptorSetLayout *out_layout)
{
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };

    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
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
        .size = sizeof(float),
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

static bool allocate_descriptor_set(pb_context *context, VkDescriptorSetLayout layout, VkDescriptorPool *out_pool, VkDescriptorSet *out_set)
{
    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
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
    pass->exposure = desc->exposure;

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
     * view + sampler change per frame, so we re-write the binding here. */
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

    vkCmdPushConstants(cmd, pass->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float), &pass->exposure);

    /* Fullscreen triangle generated by the vertex shader from gl_VertexIndex. */
    vkCmdDraw(cmd, 3, 1, 0, 0);
}
