/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "peaberry/peaberry_gltf.h"

#include "peaberry/peaberry_math.h"
#include "pbr/draw_sort.h"
#include "pbr/gltf_scene_internal.h"
#include "pbr/ibl.h"
#include "rhi/buffer.h"
#include "rhi/mesh.h"
#include "rhi/shader.h"

#include "core/log.h"
#include "pb_context_internal.h"
#include "peaberry/peaberry_vk.h"

#include <stdlib.h>
#include <string.h>

typedef struct pb_frame_ubo {
    pb_mat4 view;
    pb_mat4 proj;
    float camera_pos[3];
    float exposure;
} pb_frame_ubo;

struct pb_pbr_forward_pass {
    pb_context *context;
    VkPipeline pipeline_opaque;
    VkPipeline pipeline_blend;
    VkPipelineLayout pipeline_layout;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet *descriptor_sets;
    uint32_t descriptor_set_count;
    pb_rhi_buffer frame_buffer;
    pb_ibl_environment ibl;
    pb_gltf_scene *scene;
    VkShaderModule vert_module;
    VkShaderModule frag_module;
    float exposure;
    float prefilter_max_lod;
    /* external camera (set by pb_pbr_forward_pass_set_camera) */
    bool has_external_camera;
    pb_mat4 external_view;
    pb_mat4 external_proj;
    float external_camera_pos[3];
};

static bool create_descriptor_set_layout(struct pb_pbr_forward_pass *pass)
{
    VkDescriptorSetLayoutBinding bindings[] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 3,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 4,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 5,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 6,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 7,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 8,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 9,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 10,
        .pBindings = bindings,
    };

    VkDevice device = pb_context_device(pass->context);
    return vkCreateDescriptorSetLayout(device, &layout_info, NULL, &pass->descriptor_set_layout) == VK_SUCCESS;
}

static void destroy_scene_bindings(struct pb_pbr_forward_pass *pass)
{
    if (!pass || !pb_context_device_ready(pass->context)) {
        pass->descriptor_sets = NULL;
        pass->descriptor_set_count = 0;
        return;
    }

    VkDevice device = pb_context_device(pass->context);
    if (pass->descriptor_pool) {
        vkDestroyDescriptorPool(device, pass->descriptor_pool, NULL);
        pass->descriptor_pool = VK_NULL_HANDLE;
    }

    free(pass->descriptor_sets);
    pass->descriptor_sets = NULL;
    pass->descriptor_set_count = 0;
    pass->scene = NULL;
}

static bool write_material_descriptor_set(
    struct pb_pbr_forward_pass *pass,
    VkDescriptorSet set,
    const pb_gltf_material *material)
{
    VkDescriptorBufferInfo frame_info = {
        .buffer = pb_rhi_buffer_handle(&pass->frame_buffer),
        .offset = 0,
        .range = sizeof(pb_frame_ubo),
    };

    VkDescriptorBufferInfo material_info = {
        .buffer = pb_rhi_buffer_handle(&material->material_buffer),
        .offset = 0,
        .range = sizeof(pb_material_ubo),
    };

    VkDescriptorImageInfo albedo_info = {
        .sampler = material->albedo.sampler,
        .imageView = material->albedo.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkDescriptorImageInfo mr_info = {
        .sampler = material->metallic_roughness.sampler,
        .imageView = material->metallic_roughness.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkDescriptorImageInfo normal_info = {
        .sampler = material->normal.sampler,
        .imageView = material->normal.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkDescriptorImageInfo irradiance_info = {
        .sampler = pass->ibl.irradiance.sampler,
        .imageView = pass->ibl.irradiance.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkDescriptorImageInfo prefilter_info = {
        .sampler = pass->ibl.prefilter.sampler,
        .imageView = pass->ibl.prefilter.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkDescriptorImageInfo brdf_info = {
        .sampler = pass->ibl.brdf_lut.sampler,
        .imageView = pass->ibl.brdf_lut.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkDescriptorImageInfo occlusion_info = {
        .sampler = material->occlusion.sampler,
        .imageView = material->occlusion.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkDescriptorImageInfo emissive_info = {
        .sampler = material->emissive.sampler,
        .imageView = material->emissive.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkWriteDescriptorSet writes[] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .pBufferInfo = &frame_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .pBufferInfo = &material_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &albedo_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 3,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &mr_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 4,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &normal_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 5,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &irradiance_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 6,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &prefilter_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 7,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &brdf_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 8,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &occlusion_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 9,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &emissive_info,
        },
    };

    vkUpdateDescriptorSets(pb_context_device(pass->context), 10, writes, 0, NULL);
    return true;
}

void pb_pbr_forward_pass_set_scene(pb_pbr_forward_pass *pass, pb_gltf_scene *scene)
{
    if (!pass) {
        return;
    }

    destroy_scene_bindings(pass);

    const uint32_t material_count = scene ? pb_gltf_scene_material_count(scene) : 0;
    if (material_count == 0) {
        if (scene) {
            pb_log_error("glTF scene has no materials");
        }
        return;
    }

    VkDevice device = pb_context_device(pass->context);
    VkDescriptorSet *sets = calloc(material_count, sizeof(*sets));
    if (!sets) {
        pb_log_error("Failed to allocate %u material descriptor sets", material_count);
        return;
    }

    VkDescriptorPoolSize pool_sizes[] = {
        {
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 2 * material_count,
        },
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 8 * material_count,
        },
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = material_count,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes,
    };

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device, &pool_info, NULL, &pool) != VK_SUCCESS) {
        pb_log_error("Failed to create descriptor pool for %u materials", material_count);
        free(sets);
        return;
    }

    VkDescriptorSetLayout layout = pass->descriptor_set_layout;
    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    };

    for (uint32_t i = 0; i < material_count; ++i) {
        if (vkAllocateDescriptorSets(device, &alloc_info, &sets[i]) != VK_SUCCESS) {
            pb_log_error("Failed to allocate descriptor set %u/%u", i + 1, material_count);
            vkDestroyDescriptorPool(device, pool, NULL);
            free(sets);
            return;
        }

        write_material_descriptor_set(pass, sets[i], &scene->materials[i]);
    }

    pass->descriptor_pool = pool;
    pass->descriptor_sets = sets;
    pass->descriptor_set_count = material_count;
    pass->scene = scene;
    pb_log_info("PBR forward pass bound %u material(s)", material_count);
}

bool pb_pbr_forward_pass_scene_is_bound(const pb_pbr_forward_pass *pass)
{
    return pass && pass->scene && pass->descriptor_set_count > 0;
}

static bool create_pipeline(struct pb_pbr_forward_pass *pass, const pb_pbr_forward_pass_desc *desc)
{
    VkDevice device = pb_context_device(pass->context);

    if (!pb_rhi_shader_module_from_file(device, desc->vert_spv_path, &pass->vert_module) ||
        !pb_rhi_shader_module_from_file(device, desc->frag_spv_path, &pass->frag_module)) {
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = pass->vert_module,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = pass->frag_module,
            .pName = "main",
        },
    };

    VkVertexInputBindingDescription binding = {
        .binding = 0,
        .stride = 12 * sizeof(float),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    VkVertexInputAttributeDescription attributes[] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 3 * sizeof(float) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 6 * sizeof(float) },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 8 * sizeof(float) },
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 4,
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
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(pb_mat4),
    };

    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &pass->descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };

    if (vkCreatePipelineLayout(device, &layout_info, NULL, &pass->pipeline_layout) != VK_SUCCESS) {
        return false;
    }

    const struct {
        bool depth_write;
        bool alpha_blend;
        VkPipeline *pipeline;
    } variants[] = {
        { true, false, &pass->pipeline_opaque },
        { false, true, &pass->pipeline_blend },
    };

    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i) {
        VkPipelineDepthStencilStateCreateInfo depth_stencil = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = VK_TRUE,
            .depthWriteEnable = variants[i].depth_write ? VK_TRUE : VK_FALSE,
            .depthCompareOp = VK_COMPARE_OP_LESS,
        };

        VkPipelineColorBlendAttachmentState color_blend_attachment = {
            .colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
            .blendEnable = variants[i].alpha_blend ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .alphaBlendOp = VK_BLEND_OP_ADD,
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
            .layout = pass->pipeline_layout,
            .renderPass = desc->render_pass,
            .subpass = 0,
        };

        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, variants[i].pipeline) !=
            VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

static pb_frame_ubo build_frame_ubo(struct pb_pbr_forward_pass *pass, VkExtent2D extent)
{
    pb_frame_ubo frame = {0};

    if (pass->has_external_camera) {
        memcpy(frame.view, pass->external_view, sizeof(frame.view));
        memcpy(frame.proj, pass->external_proj, sizeof(frame.proj));
        frame.camera_pos[0] = pass->external_camera_pos[0];
        frame.camera_pos[1] = pass->external_camera_pos[1];
        frame.camera_pos[2] = pass->external_camera_pos[2];
    } else {
        const pb_vec3 eye = { 0.0f, 0.0f, 3.0f };
        const pb_vec3 center = { 0.0f, 0.0f, 0.0f };
        const pb_vec3 up = { 0.0f, 1.0f, 0.0f };
        pb_mat4_look_at(frame.view, eye, center, up);

        const float aspect = extent.height > 0 ? (float)extent.width / (float)extent.height : 1.0f;
        pb_mat4_perspective(frame.proj, pb_radians(45.0f), aspect, 0.1f, 100.0f);

        frame.camera_pos[0] = eye[0];
        frame.camera_pos[1] = eye[1];
        frame.camera_pos[2] = eye[2];
    }

    frame.exposure = pass->exposure;
    return frame;
}

static void update_frame_uniforms(struct pb_pbr_forward_pass *pass, VkExtent2D extent)
{
    const pb_frame_ubo frame = build_frame_ubo(pass, extent);
    pb_rhi_buffer_upload(pass->context, &pass->frame_buffer, &frame, sizeof(frame));
}

static void record_sorted_draws(
    struct pb_pbr_forward_pass *pass,
    VkCommandBuffer cmd,
    const pb_gltf_scene *scene,
    const pb_draw_sort_entry *entries,
    uint32_t count,
    VkPipeline pipeline,
    float time_seconds)
{
    if (count == 0) {
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    for (uint32_t i = 0; i < count; ++i) {
        const pb_gltf_draw *draw = &scene->draws[entries[i].draw_index];
        if (draw->material_index >= pass->descriptor_set_count || draw->mesh.index_count == 0) {
            continue;
        }

        VkBuffer vertex_buffer = pb_rhi_buffer_handle(&draw->mesh.vertices);
        VkBuffer index_buffer = pb_rhi_buffer_handle(&draw->mesh.indices);
        if (vertex_buffer == VK_NULL_HANDLE || index_buffer == VK_NULL_HANDLE) {
            continue;
        }

        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pass->pipeline_layout,
            0,
            1,
            &pass->descriptor_sets[draw->material_index],
            0,
            NULL);

        pb_mat4 model;
        memcpy(model, draw->world, sizeof(model));
        pb_mat4_rotate_y(model, time_seconds * 0.4f, model);

        vkCmdPushConstants(
            cmd,
            pass->pipeline_layout,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(model),
            model);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &offset);
        vkCmdBindIndexBuffer(cmd, index_buffer, 0, draw->mesh.index_type);
        vkCmdDrawIndexed(cmd, draw->mesh.index_count, 1, 0, 0, 0);
    }
}

pb_pbr_forward_pass *pb_pbr_forward_pass_create(const pb_pbr_forward_pass_desc *desc)
{
    if (!desc || !desc->context || !desc->render_pass || !desc->vert_spv_path || !desc->frag_spv_path ||
        !desc->ibl_shader_dir) {
        pb_log_error("Invalid PBR forward pass description");
        return NULL;
    }

    if (!pb_context_device_ready(desc->context)) {
        pb_log_error("Vulkan device is not initialized");
        return NULL;
    }

    pb_pbr_forward_pass *pass = calloc(1, sizeof(*pass));
    if (!pass) {
        return NULL;
    }

    pass->context = desc->context;
    pass->exposure = desc->exposure > 0.0f ? desc->exposure : 1.0f;

    pb_rhi_buffer_desc frame_desc = {
        .size = sizeof(pb_frame_ubo),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };

    if (!create_descriptor_set_layout(pass) || !pb_rhi_buffer_create(pass->context, &frame_desc, &pass->frame_buffer) ||
        !pb_ibl_environment_create(
            &(pb_ibl_environment_desc){
                .context = pass->context,
                .equirect_hdr_path = desc->ibl_equirect_hdr_path,
                .shader_dir = desc->ibl_shader_dir,
            },
            &pass->ibl) ||
        !create_pipeline(pass, desc)) {
        pb_pbr_forward_pass_destroy(pass);
        return NULL;
    }

    pass->prefilter_max_lod = pass->ibl.prefilter_max_lod;

    if (desc->scene) {
        pb_pbr_forward_pass_set_scene(pass, desc->scene);
        if (!pb_pbr_forward_pass_scene_is_bound(pass)) {
            pb_log_error("Failed to bind glTF scene to PBR forward pass");
            pb_pbr_forward_pass_destroy(pass);
            return NULL;
        }
    }

    pb_log_info("PBR forward pass ready");
    return pass;
}

void pb_pbr_forward_pass_destroy(pb_pbr_forward_pass *pass)
{
    if (!pass) {
        return;
    }

    if (pb_context_device_ready(pass->context)) {
        pb_context_wait_device_idle(pass->context);
        destroy_scene_bindings(pass);

        VkDevice device = pb_context_device(pass->context);
        if (pass->pipeline_opaque) {
            vkDestroyPipeline(device, pass->pipeline_opaque, NULL);
        }
        if (pass->pipeline_blend) {
            vkDestroyPipeline(device, pass->pipeline_blend, NULL);
        }
        if (pass->pipeline_layout) {
            vkDestroyPipelineLayout(device, pass->pipeline_layout, NULL);
        }
        if (pass->descriptor_set_layout) {
            vkDestroyDescriptorSetLayout(device, pass->descriptor_set_layout, NULL);
        }
        pb_rhi_buffer_destroy(pass->context, &pass->frame_buffer);
        pb_ibl_environment_destroy(pass->context, &pass->ibl);
        if (pass->vert_module) {
            vkDestroyShaderModule(device, pass->vert_module, NULL);
        }
        if (pass->frag_module) {
            vkDestroyShaderModule(device, pass->frag_module, NULL);
        }
    }

    free(pass);
}

void pb_pbr_forward_pass_set_camera(
    pb_pbr_forward_pass *pass,
    const pb_mat4 view,
    const pb_mat4 proj,
    const float camera_pos[3])
{
    if (!pass) {
        return;
    }

    pass->has_external_camera = true;
    memcpy(pass->external_view, view, sizeof(pass->external_view));
    memcpy(pass->external_proj, proj, sizeof(pass->external_proj));
    pass->external_camera_pos[0] = camera_pos[0];
    pass->external_camera_pos[1] = camera_pos[1];
    pass->external_camera_pos[2] = camera_pos[2];
}

void pb_pbr_forward_pass_record(
    pb_pbr_forward_pass *pass,
    VkCommandBuffer cmd,
    VkExtent2D extent,
    const pb_gltf_scene *scene,
    float time_seconds)
{
    if (!pass || !scene || extent.width == 0 || extent.height == 0) {
        return;
    }

    if (scene != pass->scene) {
        pb_pbr_forward_pass_set_scene(pass, (pb_gltf_scene *)scene);
    }

    if (!pb_pbr_forward_pass_scene_is_bound(pass) || scene != pass->scene) {
        return;
    }

    update_frame_uniforms(pass, extent);

    for (uint32_t i = 0; i < scene->material_count; ++i) {
        pb_rhi_buffer_upload(
            pass->context,
            &scene->materials[i].material_buffer,
            &scene->materials[i].material_data,
            sizeof(scene->materials[i].material_data));
    }

    const pb_frame_ubo frame = build_frame_ubo(pass, extent);
    const uint32_t draw_count = scene->draw_count;
    pb_draw_sort_entry *opaque_entries = NULL;
    pb_draw_sort_entry *blend_entries = NULL;
    uint32_t opaque_count = 0;
    uint32_t blend_count = 0;

    if (draw_count > 0) {
        opaque_entries = calloc(draw_count, sizeof(*opaque_entries));
        blend_entries = calloc(draw_count, sizeof(*blend_entries));
        if (!opaque_entries || !blend_entries) {
            free(opaque_entries);
            free(blend_entries);
            return;
        }

        for (uint32_t d = 0; d < draw_count; ++d) {
            const pb_gltf_draw *draw = &scene->draws[d];
            if (draw->material_index >= scene->material_count || draw->mesh.index_count == 0) {
                continue;
            }

            pb_mat4 model;
            memcpy(model, draw->world, sizeof(model));
            pb_mat4_rotate_y(model, time_seconds * 0.4f, model);

            const float view_depth = pb_draw_sort_view_depth(frame.view, model);
            const pb_gltf_material *material = &scene->materials[draw->material_index];
            pb_draw_sort_entry entry = {
                .draw_index = d,
                .view_depth = view_depth,
            };

            if (material->alpha_mode == PB_GLTF_ALPHA_BLEND) {
                blend_entries[blend_count++] = entry;
            } else {
                opaque_entries[opaque_count++] = entry;
            }
        }

        pb_draw_sort_stable(opaque_entries, opaque_count, false);
        pb_draw_sort_stable(blend_entries, blend_count, true);
    }

    VkViewport viewport = {
        .width = (float)extent.width,
        .height = (float)extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor = { .extent = extent };

    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    record_sorted_draws(
        pass, cmd, scene, opaque_entries, opaque_count, pass->pipeline_opaque, time_seconds);
    record_sorted_draws(
        pass, cmd, scene, blend_entries, blend_count, pass->pipeline_blend, time_seconds);

    free(opaque_entries);
    free(blend_entries);
}
