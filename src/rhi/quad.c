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
#include "rhi/buffer.h"
#include "rhi/texture.h"

#include "core/log.h"
#include "pb_context_internal.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/shader.h"

#include <stdlib.h>
#include <string.h>

typedef struct pb_quad_vertex {
    float pos[3];
    float uv[2];
} pb_quad_vertex;

typedef struct pb_quad_frame_data {
    pb_mat4 mvp;
} pb_quad_frame_data;

struct pb_quad_pass {
    pb_context *context;
    VkPipeline pipeline;
    VkPipelineLayout pipeline_layout;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    pb_rhi_buffer vertex_buffer;
    pb_rhi_buffer uniform_buffer;
    pb_rhi_texture texture;
    VkShaderModule vert_module;
    VkShaderModule frag_module;
};

static const pb_quad_vertex k_quad_vertices[] = {
    { { -0.6f, -0.6f, 0.0f }, { 0.0f, 0.0f } },
    { { 0.6f, -0.6f, 0.0f }, { 1.0f, 0.0f } },
    { { -0.6f, 0.6f, 0.0f }, { 0.0f, 1.0f } },
    { { 0.6f, -0.6f, 0.0f }, { 1.0f, 0.0f } },
    { { 0.6f, 0.6f, 0.0f }, { 1.0f, 1.0f } },
    { { -0.6f, 0.6f, 0.0f }, { 0.0f, 1.0f } },
};

static bool create_vertex_buffer(struct pb_quad_pass *pass)
{
    pb_rhi_buffer_desc desc = {
        .size = sizeof(k_quad_vertices),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };

    if (!pb_rhi_buffer_create(pass->context, &desc, &pass->vertex_buffer)) {
        return false;
    }

    return pb_rhi_buffer_upload(
        pass->context,
        &pass->vertex_buffer,
        k_quad_vertices,
        sizeof(k_quad_vertices));
}

static bool create_uniform_buffer(struct pb_quad_pass *pass)
{
    pb_rhi_buffer_desc desc = {
        .size = sizeof(pb_quad_frame_data),
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };

    return pb_rhi_buffer_create(pass->context, &desc, &pass->uniform_buffer);
}

static bool create_descriptor_set_layout(struct pb_quad_pass *pass)
{
    VkDescriptorSetLayoutBinding bindings[] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };

    VkDevice device = pb_context_device(pass->context);
    return vkCreateDescriptorSetLayout(device, &layout_info, NULL, &pass->descriptor_set_layout) == VK_SUCCESS;
}

static bool create_descriptor_pool_and_set(struct pb_quad_pass *pass)
{
    VkDevice device = pb_context_device(pass->context);

    VkDescriptorPoolSize pool_sizes[] = {
        {
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
        },
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
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

    VkDescriptorBufferInfo buffer_info = {
        .buffer = pb_rhi_buffer_handle(&pass->uniform_buffer),
        .offset = 0,
        .range = sizeof(pb_quad_frame_data),
    };

    VkDescriptorImageInfo image_info = {
        .sampler = pass->texture.sampler,
        .imageView = pass->texture.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkWriteDescriptorSet writes[] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = pass->descriptor_set,
            .dstBinding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .pBufferInfo = &buffer_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = pass->descriptor_set,
            .dstBinding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &image_info,
        },
    };

    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
    return true;
}

static bool create_pipeline(struct pb_quad_pass *pass, const pb_quad_pass_desc *desc)
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
        .stride = sizeof(pb_quad_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    VkVertexInputAttributeDescription attributes[] = {
        {
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(pb_quad_vertex, pos),
        },
        {
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = offsetof(pb_quad_vertex, uv),
        },
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 2,
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
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_FALSE,
        .depthWriteEnable = VK_FALSE,
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

static void update_frame_data(struct pb_quad_pass *pass, VkExtent2D extent, float time_seconds)
{
    pb_mat4 model;
    pb_mat4 view;
    pb_mat4 proj;
    pb_mat4 tmp;
    pb_quad_frame_data frame = {0};

    pb_mat4_identity(model);
    pb_mat4_rotate_y(model, time_seconds * 1.5f, model);

    const pb_vec3 eye = { 0.0f, 0.0f, 2.0f };
    const pb_vec3 center = { 0.0f, 0.0f, 0.0f };
    const pb_vec3 up = { 0.0f, 1.0f, 0.0f };
    pb_mat4_look_at(view, eye, center, up);

    const float aspect = extent.height > 0 ? (float)extent.width / (float)extent.height : 1.0f;
    pb_mat4_perspective(proj, pb_radians(45.0f), aspect, 0.1f, 10.0f);

    pb_mat4_mul(proj, view, tmp);
    pb_mat4_mul(tmp, model, frame.mvp);

    pb_rhi_buffer_upload(
        pass->context,
        &pass->uniform_buffer,
        &frame,
        sizeof(frame));
}

pb_quad_pass *pb_quad_pass_create(const pb_quad_pass_desc *desc)
{
    if (!desc || !desc->context || !desc->render_pass || !desc->vert_spv_path || !desc->frag_spv_path ||
        !desc->texture_path) {
        pb_log_error("Invalid quad pass description");
        return NULL;
    }

    if (!pb_context_device_ready(desc->context)) {
        pb_log_error("Vulkan device is not initialized");
        return NULL;
    }

    pb_quad_pass *pass = calloc(1, sizeof(*pass));
    if (!pass) {
        return NULL;
    }

    pass->context = desc->context;

    if (!create_descriptor_set_layout(pass) ||
        !create_uniform_buffer(pass) ||
        !pb_rhi_texture_create_from_file(pass->context, desc->texture_path, true, &pass->texture) ||
        !create_descriptor_pool_and_set(pass) ||
        !create_pipeline(pass, desc) ||
        !create_vertex_buffer(pass)) {
        pb_quad_pass_destroy(pass);
        return NULL;
    }

    pb_log_info("Quad pass ready");
    return pass;
}

void pb_quad_pass_destroy(pb_quad_pass *pass)
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
        pb_rhi_buffer_destroy(pass->context, &pass->vertex_buffer);
        pb_rhi_buffer_destroy(pass->context, &pass->uniform_buffer);
        pb_rhi_texture_destroy(pass->context, &pass->texture);
        if (pass->vert_module) {
            vkDestroyShaderModule(device, pass->vert_module, NULL);
        }
        if (pass->frag_module) {
            vkDestroyShaderModule(device, pass->frag_module, NULL);
        }
    }

    free(pass);
}

void pb_quad_pass_record(
    pb_quad_pass *pass,
    VkCommandBuffer cmd,
    VkExtent2D extent,
    float time_seconds)
{
    if (!pass || extent.width == 0 || extent.height == 0) {
        return;
    }

    update_frame_data(pass, extent, time_seconds);

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

    VkBuffer vertex_buffer = pb_rhi_buffer_handle(&pass->vertex_buffer);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &offset);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}
