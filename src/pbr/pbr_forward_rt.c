/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pbr/pbr_forward_rt.h"

#ifdef PEABERRY_ENABLE_RAYTRACING

#include "core/log.h"
#include "pbr/gltf_scene_internal.h"
#include "pbr/rt_scene.h"
#include "pbr/vertex.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/shader.h"

#include <stdio.h>
#include <string.h>

static bool create_rt_pipeline_variant(
    pb_pbr_forward_rt *rt,
    VkRenderPass render_pass,
    VkSampleCountFlagBits samples,
    VkCullModeFlags cull_mode,
    VkPipeline *out_pipeline)
{
    VkDevice device = pb_context_device(rt->context);

    VkPipelineShaderStageCreateInfo stages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = rt->vert_module,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = rt->frag_module,
            .pName = "main",
        },
    };

    VkVertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(pb_pbr_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    VkVertexInputAttributeDescription attributes[] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(pb_pbr_vertex, pos) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(pb_pbr_vertex, normal) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(pb_pbr_vertex, uv) },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(pb_pbr_vertex, tangent) },
        { .location = 4, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(pb_pbr_vertex, joints) },
        { .location = 5, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(pb_pbr_vertex, weights) },
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 6,
        .pVertexAttributeDescriptions = attributes,
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

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };

    VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = cull_mode,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = samples != 0 ? samples : VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
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
        .layout = rt->pipeline_layout,
        .renderPass = render_pass,
        .subpass = 0,
    };

    return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, out_pipeline) == VK_SUCCESS;
}

bool pb_pbr_forward_rt_create(
    pb_pbr_forward_rt *rt,
    pb_context *context,
    VkShaderModule vert_module,
    VkPipelineLayout pipeline_layout,
    VkRenderPass render_pass,
    VkSampleCountFlagBits samples,
    const char *rt_frag_spv_path)
{
    memset(rt, 0, sizeof(*rt));
    rt->context = context;
    rt->vert_module = vert_module;
    rt->pipeline_layout = pipeline_layout;
    rt->available = context && pb_context_raytracing_supported(context);

    if (!rt->available || !rt_frag_spv_path) {
        return true;
    }

    VkDevice device = pb_context_device(context);
    if (!pb_rhi_shader_module_from_file(device, rt_frag_spv_path, &rt->frag_module)) {
        pb_log_error("Failed to load RT fragment shader");
        return false;
    }

    if (!create_rt_pipeline_variant(
            rt, render_pass, samples, VK_CULL_MODE_BACK_BIT, &rt->pipeline_opaque) ||
        !create_rt_pipeline_variant(
            rt, render_pass, samples, VK_CULL_MODE_NONE, &rt->pipeline_opaque_double)) {
        pb_pbr_forward_rt_destroy(rt);
        return false;
    }

    pb_log_info("Hybrid rayQuery reflection pipeline ready");
    return true;
}

void pb_pbr_forward_rt_destroy(pb_pbr_forward_rt *rt)
{
    if (!rt || !rt->context || !pb_context_device_ready(rt->context)) {
        return;
    }

    VkDevice device = pb_context_device(rt->context);
    if (rt->pipeline_opaque) {
        vkDestroyPipeline(device, rt->pipeline_opaque, NULL);
    }
    if (rt->pipeline_opaque_double) {
        vkDestroyPipeline(device, rt->pipeline_opaque_double, NULL);
    }
    if (rt->frag_module) {
        vkDestroyShaderModule(device, rt->frag_module, NULL);
    }

    if (rt->scene) {
        pb_rt_scene_destroy(rt->scene);
        rt->scene = NULL;
    }

    memset(rt, 0, sizeof(*rt));
}

void pb_pbr_forward_rt_set_scene(pb_pbr_forward_rt *rt, pb_gltf_scene *scene)
{
    if (!rt || !rt->available) {
        return;
    }

    if (rt->scene) {
        pb_rt_scene_destroy(rt->scene);
        rt->scene = NULL;
    }

    if (scene) {
        rt->scene = pb_rt_scene_create(rt->context, scene);
        if (!rt->scene) {
            pb_log_warn("Failed to build RT acceleration structures for scene");
        }
    }
}

void pb_pbr_forward_rt_set_enabled(pb_pbr_forward_rt *rt, bool enabled)
{
    if (!rt) {
        return;
    }

    rt->enabled = enabled && rt->available && rt->scene != NULL;
}

bool pb_pbr_forward_rt_available(const pb_pbr_forward_rt *rt)
{
    return rt && rt->available;
}

void pb_pbr_forward_rt_update_scene(pb_pbr_forward_rt *rt, const pb_gltf_scene *scene)
{
    if (!rt || !rt->scene || !scene) {
        return;
    }

    if (!pb_rt_scene_update(rt->scene, scene)) {
        pb_log_warn("RT TLAS update failed");
    }
}

void pb_pbr_forward_rt_write_descriptor(pb_pbr_forward_rt *rt, VkDescriptorSet set)
{
    if (!rt || !rt->scene) {
        return;
    }

    VkAccelerationStructureKHR tlas = pb_rt_scene_tlas(rt->scene);
    if (tlas == VK_NULL_HANDLE) {
        return;
    }

    VkWriteDescriptorSetAccelerationStructureKHR as_info = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures = &tlas,
    };

    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = &as_info,
        .dstSet = set,
        .dstBinding = 13,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
    };

    vkUpdateDescriptorSets(pb_context_device(rt->context), 1, &write, 0, NULL);
}

VkPipeline pb_pbr_forward_rt_select_opaque_pipeline(
    const pb_pbr_forward_rt *rt,
    VkPipeline fallback,
    VkPipeline fallback_double,
    bool double_sided)
{
    if (!rt || !rt->enabled) {
        return double_sided ? fallback_double : fallback;
    }

    return double_sided ? rt->pipeline_opaque_double : rt->pipeline_opaque;
}

bool pb_pbr_forward_rt_uses_rt_pipeline(const pb_pbr_forward_rt *rt, VkPipeline pipeline)
{
    if (!rt || !rt->enabled) {
        return false;
    }

    return pipeline == rt->pipeline_opaque || pipeline == rt->pipeline_opaque_double;
}

uint32_t pb_pbr_forward_rt_extra_binding_count(void)
{
    return 1;
}

#endif
