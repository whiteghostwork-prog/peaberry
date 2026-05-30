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

#include "core/log.h"
#include "rhi/shader.h"

#include <stdlib.h>
#include <string.h>

typedef struct pb_triangle_vertex {
    float pos[2];
    float color[3];
} pb_triangle_vertex;

struct pb_triangle_pass {
    pb_context *context;
    VkPipeline pipeline;
    VkPipelineLayout pipeline_layout;
    VkBuffer vertex_buffer;
    VkDeviceMemory vertex_memory;
    VkShaderModule vert_module;
    VkShaderModule frag_module;
};

static const pb_triangle_vertex k_triangle_vertices[] = {
    { { 0.0f, -0.5f }, { 1.0f, 0.0f, 0.0f } },
    { { 0.5f, 0.5f }, { 0.0f, 1.0f, 0.0f } },
    { { -0.5f, 0.5f }, { 0.0f, 0.0f, 1.0f } },
};

static bool create_vertex_buffer(struct pb_triangle_pass *pass)
{
    VkDevice device = pb_context_device(pass->context);
    VkDeviceSize size = sizeof(k_triangle_vertices);

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    if (vkCreateBuffer(device, &buffer_info, NULL, &pass->vertex_buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements mem_requirements;
    vkGetBufferMemoryRequirements(device, pass->vertex_buffer, &mem_requirements);

    uint32_t memory_type_index = UINT32_MAX;
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(pb_context_physical_device(pass->context), &mem_properties);

    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
        if ((mem_requirements.memoryTypeBits & (1u << i)) &&
            (mem_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mem_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memory_type_index = i;
            break;
        }
    }

    if (memory_type_index == UINT32_MAX) {
        return false;
    }

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_requirements.size,
        .memoryTypeIndex = memory_type_index,
    };

    if (vkAllocateMemory(device, &alloc_info, NULL, &pass->vertex_memory) != VK_SUCCESS) {
        return false;
    }

    if (vkBindBufferMemory(device, pass->vertex_buffer, pass->vertex_memory, 0) != VK_SUCCESS) {
        return false;
    }

    void *mapped = NULL;
    if (vkMapMemory(device, pass->vertex_memory, 0, size, 0, &mapped) != VK_SUCCESS) {
        return false;
    }

    memcpy(mapped, k_triangle_vertices, sizeof(k_triangle_vertices));
    vkUnmapMemory(device, pass->vertex_memory);
    return true;
}

static bool create_pipeline(struct pb_triangle_pass *pass, const pb_triangle_pass_desc *desc)
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
        .stride = sizeof(pb_triangle_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    VkVertexInputAttributeDescription attributes[] = {
        {
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offset = offsetof(pb_triangle_vertex, pos),
        },
        {
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(pb_triangle_vertex, color),
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

pb_triangle_pass *pb_triangle_pass_create(const pb_triangle_pass_desc *desc)
{
    if (!desc || !desc->context || !desc->render_pass || !desc->vert_spv_path || !desc->frag_spv_path) {
        pb_log_error("Invalid triangle pass description");
        return NULL;
    }

    if (!pb_context_device_ready(desc->context)) {
        pb_log_error("Vulkan device is not initialized");
        return NULL;
    }

    pb_triangle_pass *pass = calloc(1, sizeof(*pass));
    if (!pass) {
        return NULL;
    }

    pass->context = desc->context;

    if (!create_pipeline(pass, desc) || !create_vertex_buffer(pass)) {
        pb_triangle_pass_destroy(pass);
        return NULL;
    }

    pb_log_info("Triangle pass ready");
    return pass;
}

void pb_triangle_pass_destroy(pb_triangle_pass *pass)
{
    if (!pass) {
        return;
    }

    if (pb_context_device_ready(pass->context)) {
        VkDevice device = pb_context_device(pass->context);

        if (pass->pipeline) {
            vkDestroyPipeline(device, pass->pipeline, NULL);
        }
        if (pass->pipeline_layout) {
            vkDestroyPipelineLayout(device, pass->pipeline_layout, NULL);
        }
        if (pass->vertex_buffer) {
            vkDestroyBuffer(device, pass->vertex_buffer, NULL);
        }
        if (pass->vertex_memory) {
            vkFreeMemory(device, pass->vertex_memory, NULL);
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

void pb_triangle_pass_record(pb_triangle_pass *pass, VkCommandBuffer cmd, VkExtent2D extent)
{
    if (!pass || extent.width == 0 || extent.height == 0) {
        return;
    }

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

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &pass->vertex_buffer, &offset);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}
