/*
 * Copyright 2026 The Peaberry Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "peaberry/peaberry_render.h"

#include "peaberry/peaberry_math.h"
#include "pbr/ibl.h"
#include "rhi/buffer.h"
#include "rhi/mesh.h"
#include "rhi/texture.h"

#include "core/log.h"
#include "pb_context_internal.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/shader.h"

#include <stdlib.h>
#include <string.h>

typedef struct pb_frame_ubo {
    pb_mat4 model;
    pb_mat4 view;
    pb_mat4 proj;
    float camera_pos[3];
    float exposure;
} pb_frame_ubo;

typedef struct pb_material_ubo {
    float light_dir[3];
    float _pad0;
    float albedo_factor[3];
    float metallic_factor;
    float light_color[3];
    float roughness_factor;
} pb_material_ubo;

struct pb_sphere_pass {
    pb_context *context;
    VkPipeline pipeline;
    VkPipelineLayout pipeline_layout;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    pb_rhi_buffer frame_buffer;
    pb_rhi_buffer material_buffer;
    pb_rhi_texture albedo_texture;
    pb_rhi_texture metallic_roughness_texture;
    pb_rhi_texture normal_texture;
    pb_ibl_environment ibl;
    pb_rhi_mesh mesh;
    VkShaderModule vert_module;
    VkShaderModule frag_module;
    pb_material_ubo material;
    float exposure;
    float prefilter_max_lod;
};

static bool create_uniform_buffers(struct pb_sphere_pass *pass)
{
    pb_rhi_buffer_desc frame_desc = {
        .size = sizeof(pb_frame_ubo),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };

    pb_rhi_buffer_desc material_desc = {
        .size = sizeof(pb_material_ubo),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };

    return pb_rhi_buffer_create(pass->context, &frame_desc, &pass->frame_buffer) &&
           pb_rhi_buffer_create(pass->context, &material_desc, &pass->material_buffer);
}

static bool create_descriptor_set_layout(struct pb_sphere_pass *pass)
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
    };

    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 8,
        .pBindings = bindings,
    };

    VkDevice device = pb_context_device(pass->context);
    return vkCreateDescriptorSetLayout(device, &layout_info, NULL, &pass->descriptor_set_layout) == VK_SUCCESS;
}

static bool create_descriptor_pool_and_set(struct pb_sphere_pass *pass)
{
    VkDevice device = pb_context_device(pass->context);

    VkDescriptorPoolSize pool_sizes[] = {
        {
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 2,
        },
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 6,
        },
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes,
    };

    if (vkCreateDescriptorPool(device, &pool_info, NULL, &pass->descriptor_pool) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pass->descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &pass->descriptor_set_layout,
    };

    if (vkAllocateDescriptorSets(device, &alloc_info, &pass->descriptor_set) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorBufferInfo frame_info = {
        .buffer = pb_rhi_buffer_handle(&pass->frame_buffer),
        .offset = 0,
        .range = sizeof(pb_frame_ubo),
    };

    VkDescriptorBufferInfo material_info = {
        .buffer = pb_rhi_buffer_handle(&pass->material_buffer),
        .offset = 0,
        .range = sizeof(pb_material_ubo),
    };

    VkDescriptorImageInfo albedo_info = {
        .sampler = pass->albedo_texture.sampler,
        .imageView = pass->albedo_texture.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkDescriptorImageInfo mr_info = {
        .sampler = pass->metallic_roughness_texture.sampler,
        .imageView = pass->metallic_roughness_texture.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkDescriptorImageInfo normal_info = {
        .sampler = pass->normal_texture.sampler,
        .imageView = pass->normal_texture.view,
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

    VkWriteDescriptorSet writes[] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = pass->descriptor_set,
            .dstBinding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .pBufferInfo = &frame_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = pass->descriptor_set,
            .dstBinding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .pBufferInfo = &material_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = pass->descriptor_set,
            .dstBinding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &albedo_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = pass->descriptor_set,
            .dstBinding = 3,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &mr_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = pass->descriptor_set,
            .dstBinding = 4,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &normal_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = pass->descriptor_set,
            .dstBinding = 5,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &irradiance_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = pass->descriptor_set,
            .dstBinding = 6,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &prefilter_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = pass->descriptor_set,
            .dstBinding = 7,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &brdf_info,
        },
    };

    vkUpdateDescriptorSets(device, 8, writes, 0, NULL);
    return true;
}

static bool create_pipeline(struct pb_sphere_pass *pass, const pb_sphere_pass_desc *desc)
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
        {
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = 0,
        },
        {
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = 3 * sizeof(float),
        },
        {
            .location = 2,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = 6 * sizeof(float),
        },
        {
            .location = 3,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offset = 8 * sizeof(float),
        },
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
        .primitiveRestartEnable = VK_FALSE,
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

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo color_blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
    };

    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &pass->descriptor_set_layout,
    };

    if (vkCreatePipelineLayout(device, &layout_info, NULL, &pass->pipeline_layout) != VK_SUCCESS) {
        return false;
    }

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

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pass->pipeline) != VK_SUCCESS) {
        return false;
    }

    return true;
}

static void update_uniforms(struct pb_sphere_pass *pass, VkExtent2D extent, float time_seconds)
{
    pb_frame_ubo frame = {0};

    pb_mat4_identity(frame.model);
    pb_mat4_rotate_y(frame.model, time_seconds * 0.4f, frame.model);

    const pb_vec3 eye = { 0.0f, 0.0f, 3.0f };
    const pb_vec3 center = { 0.0f, 0.0f, 0.0f };
    const pb_vec3 up = { 0.0f, 1.0f, 0.0f };
    pb_mat4_look_at(frame.view, eye, center, up);

    const float aspect = extent.height > 0 ? (float)extent.width / (float)extent.height : 1.0f;
    pb_mat4_perspective(frame.proj, pb_radians(45.0f), aspect, 0.1f, 100.0f);

    frame.camera_pos[0] = eye[0];
    frame.camera_pos[1] = eye[1];
    frame.camera_pos[2] = eye[2];
    frame.exposure = pass->exposure;

    pb_rhi_buffer_upload(pass->context, &pass->frame_buffer, &frame, sizeof(frame));
    pb_rhi_buffer_upload(pass->context, &pass->material_buffer, &pass->material, sizeof(pass->material));
}

pb_sphere_pass *pb_sphere_pass_create(const pb_sphere_pass_desc *desc)
{
    if (!desc || !desc->context || !desc->render_pass || !desc->vert_spv_path || !desc->frag_spv_path ||
        !desc->albedo_texture_path || !desc->metallic_roughness_texture_path || !desc->normal_texture_path ||
        !desc->ibl_shader_dir) {
        pb_log_error("Invalid PBR sphere pass description");
        return NULL;
    }

    if (!pb_context_device_ready(desc->context)) {
        pb_log_error("Vulkan device is not initialized");
        return NULL;
    }

    pb_sphere_pass *pass = calloc(1, sizeof(*pass));
    if (!pass) {
        return NULL;
    }

    pass->context = desc->context;
    pass->material.albedo_factor[0] = desc->albedo_factor[0];
    pass->material.albedo_factor[1] = desc->albedo_factor[1];
    pass->material.albedo_factor[2] = desc->albedo_factor[2];
    pass->material.metallic_factor = desc->metallic_factor;
    pass->material.roughness_factor = desc->roughness_factor;
    pass->material.light_color[0] = 4.0f;
    pass->material.light_color[1] = 4.0f;
    pass->material.light_color[2] = 4.0f;
    /* Direction from origin toward the light (upper-right-front). */
    pass->material.light_dir[0] = 0.5f;
    pass->material.light_dir[1] = 0.8f;
    pass->material.light_dir[2] = 0.4f;
    pass->exposure = desc->exposure > 0.0f ? desc->exposure : 1.0f;

    pb_rhi_mesh_uv_sphere_desc mesh_desc = {
        .radius = 1.0f,
        .sectors = 48,
        .stacks = 32,
    };

    if (!create_descriptor_set_layout(pass) ||
        !create_uniform_buffers(pass) ||
        !pb_rhi_texture_create_from_file(
            pass->context, desc->albedo_texture_path, true, &pass->albedo_texture) ||
        !pb_rhi_texture_create_from_file(
            pass->context, desc->metallic_roughness_texture_path, false, &pass->metallic_roughness_texture) ||
        !pb_rhi_texture_create_from_file(
            pass->context, desc->normal_texture_path, false, &pass->normal_texture) ||
        !pb_ibl_environment_create(
            &(pb_ibl_environment_desc){
                .context = pass->context,
                .equirect_hdr_path = desc->ibl_equirect_hdr_path,
                .shader_dir = desc->ibl_shader_dir,
            },
            &pass->ibl) ||
        !create_descriptor_pool_and_set(pass) ||
        !pb_rhi_mesh_create_uv_sphere(pass->context, &mesh_desc, &pass->mesh) ||
        !create_pipeline(pass, desc)) {
        pb_sphere_pass_destroy(pass);
        return NULL;
    }

    pass->prefilter_max_lod = pass->ibl.prefilter_max_lod;

    pb_log_info("PBR sphere pass ready");
    return pass;
}

void pb_sphere_pass_destroy(pb_sphere_pass *pass)
{
    if (!pass) {
        return;
    }

    if (pb_context_device_ready(pass->context)) {
        pb_context_wait_device_idle(pass->context);

        VkDevice device = pb_context_device(pass->context);

        if (pass->pipeline) {
            vkDestroyPipeline(device, pass->pipeline, NULL);
        }
        if (pass->pipeline_layout) {
            vkDestroyPipelineLayout(device, pass->pipeline_layout, NULL);
        }
        if (pass->descriptor_pool) {
            vkDestroyDescriptorPool(device, pass->descriptor_pool, NULL);
        }
        if (pass->descriptor_set_layout) {
            vkDestroyDescriptorSetLayout(device, pass->descriptor_set_layout, NULL);
        }
        pb_rhi_buffer_destroy(pass->context, &pass->frame_buffer);
        pb_rhi_buffer_destroy(pass->context, &pass->material_buffer);
        pb_rhi_texture_destroy(pass->context, &pass->albedo_texture);
        pb_rhi_texture_destroy(pass->context, &pass->metallic_roughness_texture);
        pb_rhi_texture_destroy(pass->context, &pass->normal_texture);
        pb_ibl_environment_destroy(pass->context, &pass->ibl);
        pb_rhi_mesh_destroy(pass->context, &pass->mesh);
        if (pass->vert_module) {
            vkDestroyShaderModule(device, pass->vert_module, NULL);
        }
        if (pass->frag_module) {
            vkDestroyShaderModule(device, pass->frag_module, NULL);
        }
    }

    free(pass);
}

void pb_sphere_pass_record(
    pb_sphere_pass *pass,
    VkCommandBuffer cmd,
    VkExtent2D extent,
    float time_seconds)
{
    if (!pass || extent.width == 0 || extent.height == 0) {
        return;
    }

    update_uniforms(pass, extent, time_seconds);

    VkViewport viewport = {
        .width = (float)extent.width,
        .height = (float)extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor = {
        .extent = extent,
    };

    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
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

    VkBuffer vertex_buffer = pb_rhi_buffer_handle(&pass->mesh.vertices);
    VkBuffer index_buffer = pb_rhi_buffer_handle(&pass->mesh.indices);
    VkDeviceSize offset = 0;

    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &offset);
    vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, pass->mesh.index_count, 1, 0, 0, 0);
}
