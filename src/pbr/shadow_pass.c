/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pbr/shadow_pass.h"

#include "core/log.h"
#include "pbr/vertex.h"
#include "pb_context_internal.h"
#include "peaberry/peaberry_math.h"
#include "rhi/alloc.h"
#include "rhi/buffer.h"
#include "rhi/shader.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct pb_shadow_pass {
    pb_context *context;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkPipeline pipeline_double_sided;
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkImage depth_image;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;
    VkSampler sampler;
    VkFormat depth_format;
    VkShaderModule vert_module;
    VkShaderModule frag_module;
};

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
        if ((props.optimalTilingFeatures &
                (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) ==
            (VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            *out_format = candidates[i];
            return true;
        }
    }

    return false;
}

static bool create_depth_resources(pb_shadow_pass *pass)
{
    VkDevice device = pb_context_device(pass->context);
    VkPhysicalDevice physical_device = pb_context_physical_device(pass->context);

    if (!choose_depth_format(physical_device, &pass->depth_format)) {
        pb_log_error("No suitable depth format for shadow map");
        return false;
    }

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = pass->depth_format,
        .extent = { PB_SHADOW_MAP_SIZE, PB_SHADOW_MAP_SIZE, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (vkCreateImage(device, &image_info, NULL, &pass->depth_image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, pass->depth_image, &mem_reqs);

    const pb_vk_context *vk = &pass->context->vk;
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

    if (vkAllocateMemory(device, &alloc_info, NULL, &pass->depth_memory) != VK_SUCCESS) {
        return false;
    }

    if (vkBindImageMemory(device, pass->depth_image, pass->depth_memory, 0) != VK_SUCCESS) {
        return false;
    }

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = pass->depth_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = pass->depth_format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };

    if (vkCreateImageView(device, &view_info, NULL, &pass->depth_view) != VK_SUCCESS) {
        return false;
    }

    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .compareEnable = VK_TRUE,
        .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
    };

    return vkCreateSampler(device, &sampler_info, NULL, &pass->sampler) == VK_SUCCESS;
}

static bool create_render_pass(pb_shadow_pass *pass)
{
    VkAttachmentDescription depth_attachment = {
        .format = pass->depth_format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
    };

    VkAttachmentReference depth_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .pDepthStencilAttachment = &depth_ref,
    };

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &depth_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };

    return vkCreateRenderPass(pb_context_device(pass->context), &render_pass_info, NULL, &pass->render_pass) ==
        VK_SUCCESS;
}

static bool create_framebuffer(pb_shadow_pass *pass)
{
    VkFramebufferCreateInfo fb_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = pass->render_pass,
        .attachmentCount = 1,
        .pAttachments = &pass->depth_view,
        .width = PB_SHADOW_MAP_SIZE,
        .height = PB_SHADOW_MAP_SIZE,
        .layers = 1,
    };

    return vkCreateFramebuffer(pb_context_device(pass->context), &fb_info, NULL, &pass->framebuffer) == VK_SUCCESS;
}

static bool create_pipeline_variant(
    pb_shadow_pass *pass,
    VkCullModeFlags cull_mode,
    VkPipeline *out_pipeline)
{
    VkDevice device = pb_context_device(pass->context);

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
        .stride = sizeof(pb_pbr_vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    VkVertexInputAttributeDescription attributes[] = {
        {
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = offsetof(pb_pbr_vertex, pos),
        },
        {
            .location = 4,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offset = offsetof(pb_pbr_vertex, joints),
        },
        {
            .location = 5,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offset = offsetof(pb_pbr_vertex, weights),
        },
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 3,
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
        /* Cull front faces (render back faces) for the opaque shadow pipeline.
         * Storing the FAR side of closed geometry means the near (lit) faces
         * are always closer to the light than the stored depth, so they pass
         * the shadow test cleanly — no self-occlusion acne, and because the
         * near and far faces are separated by the full geometry depth (not a
         * fragile bias), PCF neighbors can't bleed shadow across silhouette
         * edges onto lit faces. The double-sided variant keeps its cull_mode
         * for non-watertight geometry. */
        .cullMode = cull_mode == VK_CULL_MODE_BACK_BIT
            ? VK_CULL_MODE_FRONT_BIT
            : cull_mode,
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
        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
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
        .pDynamicState = &dynamic_state,
        .layout = pass->pipeline_layout,
        .renderPass = pass->render_pass,
        .subpass = 0,
    };

    return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, out_pipeline) == VK_SUCCESS;
}

static void expand_bounds_point(
    const pb_mat4 world,
    const float local[3],
    float bounds_min[3],
    float bounds_max[3])
{
    const float x = world[0][0] * local[0] + world[1][0] * local[1] + world[2][0] * local[2] + world[3][0];
    const float y = world[0][1] * local[0] + world[1][1] * local[1] + world[2][1] * local[2] + world[3][1];
    const float z = world[0][2] * local[0] + world[1][2] * local[1] + world[2][2] * local[2] + world[3][2];

    if (x < bounds_min[0]) {
        bounds_min[0] = x;
    }
    if (y < bounds_min[1]) {
        bounds_min[1] = y;
    }
    if (z < bounds_min[2]) {
        bounds_min[2] = z;
    }
    if (x > bounds_max[0]) {
        bounds_max[0] = x;
    }
    if (y > bounds_max[1]) {
        bounds_max[1] = y;
    }
    if (z > bounds_max[2]) {
        bounds_max[2] = z;
    }
}

void pb_shadow_scene_bounds(const pb_gltf_scene *scene, float bounds_min[3], float bounds_max[3])
{
    bounds_min[0] = bounds_min[1] = bounds_min[2] = FLT_MAX;
    bounds_max[0] = bounds_max[1] = bounds_max[2] = -FLT_MAX;

    if (!scene) {
        bounds_min[0] = bounds_min[1] = bounds_min[2] = -1.0f;
        bounds_max[0] = bounds_max[1] = bounds_max[2] = 1.0f;
        return;
    }

    for (uint32_t d = 0; d < scene->draw_count; ++d) {
        const pb_gltf_draw *draw = &scene->draws[d];
        if (draw->mesh.index_count == 0) {
            continue;
        }

        for (uint32_t corner = 0; corner < 8; ++corner) {
            const float local[3] = {
                (corner & 1) ? draw->bounds_max[0] : draw->bounds_min[0],
                (corner & 2) ? draw->bounds_max[1] : draw->bounds_min[1],
                (corner & 4) ? draw->bounds_max[2] : draw->bounds_min[2],
            };
            expand_bounds_point(draw->world, local, bounds_min, bounds_max);
        }
    }

    if (bounds_min[0] > bounds_max[0]) {
        bounds_min[0] = bounds_min[1] = bounds_min[2] = -1.0f;
        bounds_max[0] = bounds_max[1] = bounds_max[2] = 1.0f;
    }
}

bool pb_shadow_light_matrices_fit_aabb(
    const float light_dir[3],
    const float bounds_min[3],
    const float bounds_max[3],
    pb_mat4 out_light_view,
    pb_mat4 out_light_proj)
{
    pb_vec3 center = {
        0.5f * (bounds_min[0] + bounds_max[0]),
        0.5f * (bounds_min[1] + bounds_max[1]),
        0.5f * (bounds_min[2] + bounds_max[2]),
    };

    const float extent_x = bounds_max[0] - bounds_min[0];
    const float extent_y = bounds_max[1] - bounds_min[1];
    const float extent_z = bounds_max[2] - bounds_min[2];
    const float radius = 0.5f * sqrtf(extent_x * extent_x + extent_y * extent_y + extent_z * extent_z);

    pb_vec3 dir = { light_dir[0], light_dir[1], light_dir[2] };
    pb_vec3 dir_norm;
    pb_vec3_normalize(dir, dir_norm);

    pb_vec3 eye = {
        center[0] - dir_norm[0] * (radius * 2.0f + 5.0f),
        center[1] - dir_norm[1] * (radius * 2.0f + 5.0f),
        center[2] - dir_norm[2] * (radius * 2.0f + 5.0f),
    };

    pb_vec3 up = { 0.0f, 1.0f, 0.0f };
    if (fabsf(dir_norm[1]) > 0.95f) {
        up[0] = 0.0f;
        up[1] = 0.0f;
        up[2] = 1.0f;
    }

    pb_mat4_look_at(out_light_view, eye, center, up);

    float light_min[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float light_max[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

    for (uint32_t corner = 0; corner < 8; ++corner) {
        const float world[3] = {
            (corner & 1) ? bounds_max[0] : bounds_min[0],
            (corner & 2) ? bounds_max[1] : bounds_min[1],
            (corner & 4) ? bounds_max[2] : bounds_min[2],
        };

        const float x = out_light_view[0][0] * world[0] + out_light_view[1][0] * world[1] +
            out_light_view[2][0] * world[2] + out_light_view[3][0];
        const float y = out_light_view[0][1] * world[0] + out_light_view[1][1] * world[1] +
            out_light_view[2][1] * world[2] + out_light_view[3][1];
        const float z = out_light_view[0][2] * world[0] + out_light_view[1][2] * world[1] +
            out_light_view[2][2] * world[2] + out_light_view[3][2];

        if (x < light_min[0]) {
            light_min[0] = x;
        }
        if (y < light_min[1]) {
            light_min[1] = y;
        }
        if (z < light_min[2]) {
            light_min[2] = z;
        }
        if (x > light_max[0]) {
            light_max[0] = x;
        }
        if (y > light_max[1]) {
            light_max[1] = y;
        }
        if (z > light_max[2]) {
            light_max[2] = z;
        }
    }

    const float pad = 0.05f * fmaxf(light_max[0] - light_min[0], light_max[1] - light_min[1]);
    pb_mat4_ortho(
        out_light_proj,
        light_min[0] - pad,
        light_max[0] + pad,
        light_min[1] - pad,
        light_max[1] + pad,
        light_min[2] - pad,
        light_max[2] + pad);

    return true;
}

static void record_shadow_draw(
    pb_shadow_pass *pass,
    VkCommandBuffer cmd,
    const pb_gltf_scene *scene,
    uint32_t draw_index,
    const pb_gltf_draw *draw,
    VkPipeline pipeline,
    VkDescriptorSet descriptor_set,
    const uint32_t *dynamic_offsets,
    uint32_t dynamic_offset_count,
    uint32_t instance_count)
{
    if (draw->material_index >= scene->material_count || draw->mesh.index_count == 0 || instance_count == 0) {
        return;
    }

    const pb_gltf_material *material = &scene->materials[draw->material_index];
    if (material->alpha_mode == PB_GLTF_ALPHA_BLEND) {
        return;
    }

    VkBuffer vertex_buffer = pb_rhi_buffer_handle(&draw->mesh.vertices);
    VkBuffer index_buffer = pb_rhi_buffer_handle(&draw->mesh.indices);
    if (vertex_buffer == VK_NULL_HANDLE || index_buffer == VK_NULL_HANDLE) {
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        pass->pipeline_layout,
        0,
        1,
        &descriptor_set,
        dynamic_offset_count,
        dynamic_offsets);

    pb_pbr_push_constants push = {0};
    const bool use_instancing = instance_count > 1;
    if (use_instancing) {
        pb_mat4_identity(push.model);
        push.instanced = 1u;
    } else {
        memcpy(push.model, draw->world, sizeof(push.model));
        push.skinned = draw->skin_index != PB_GLTF_NO_SKIN ? 1u : 0u;
        push.palette_base = draw_index * PB_GLTF_SKIN_JOINTS_MAX;
    }

    vkCmdPushConstants(
        cmd,
        pass->pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT,
        0,
        sizeof(push),
        &push);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &offset);
    vkCmdBindIndexBuffer(cmd, index_buffer, 0, draw->mesh.index_type);
    vkCmdDrawIndexed(cmd, draw->mesh.index_count, instance_count, 0, 0, 0);
}

void pb_shadow_pass_record(
    pb_shadow_pass *pass,
    VkCommandBuffer cmd,
    const pb_gltf_scene *scene,
    VkDescriptorSet *material_descriptor_sets,
    uint32_t descriptor_set_count,
    const uint32_t *dynamic_offsets,
    uint32_t dynamic_offset_count,
    uint32_t instanced_draw_index,
    uint32_t instanced_count)
{
    if (!pass || !scene || !cmd || !material_descriptor_sets || descriptor_set_count == 0) {
        return;
    }

    const bool has_instanced = instanced_count > 0 && instanced_draw_index < scene->draw_count;

    const VkExtent2D extent = { PB_SHADOW_MAP_SIZE, PB_SHADOW_MAP_SIZE };
    VkClearValue clear = { .depthStencil = { 1.0f, 0 } };

    VkRenderPassBeginInfo rp_begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = pass->render_pass,
        .framebuffer = pass->framebuffer,
        .renderArea = { .extent = extent },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };

    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {
        .width = (float)extent.width,
        .height = (float)extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor = { .extent = extent };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    for (uint32_t d = 0; d < scene->draw_count; ++d) {
        if (has_instanced && d == instanced_draw_index) {
            continue;
        }

        const pb_gltf_draw *draw = &scene->draws[d];
        if (draw->material_index >= descriptor_set_count) {
            continue;
        }

        const pb_gltf_material *material = &scene->materials[draw->material_index];
        const VkPipeline pipeline =
            material->double_sided ? pass->pipeline_double_sided : pass->pipeline;
        record_shadow_draw(
            pass,
            cmd,
            scene,
            d,
            draw,
            pipeline,
            material_descriptor_sets[draw->material_index],
            dynamic_offsets,
            dynamic_offset_count,
            1);
    }

    if (has_instanced) {
        const pb_gltf_draw *draw = &scene->draws[instanced_draw_index];
        if (draw->material_index < descriptor_set_count) {
            const pb_gltf_material *material = &scene->materials[draw->material_index];
            const VkPipeline pipeline =
                material->double_sided ? pass->pipeline_double_sided : pass->pipeline;
            record_shadow_draw(
                pass,
                cmd,
                scene,
                instanced_draw_index,
                draw,
                pipeline,
                material_descriptor_sets[draw->material_index],
                dynamic_offsets,
                dynamic_offset_count,
                instanced_count);
        }
    }

    vkCmdEndRenderPass(cmd);

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = pass->depth_image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0,
        NULL,
        0,
        NULL,
        1,
        &barrier);
}

VkImageView pb_shadow_pass_depth_view(const pb_shadow_pass *pass)
{
    return pass ? pass->depth_view : VK_NULL_HANDLE;
}

VkSampler pb_shadow_pass_sampler(const pb_shadow_pass *pass)
{
    return pass ? pass->sampler : VK_NULL_HANDLE;
}

bool pb_shadow_pass_create(const pb_shadow_pass_desc *desc, pb_shadow_pass **out_pass)
{
    if (!desc || !desc->context || !desc->pipeline_layout || !desc->vert_spv_path || !desc->frag_spv_path ||
        !out_pass) {
        return false;
    }

    if (!pb_context_device_ready(desc->context)) {
        return false;
    }

    pb_shadow_pass *pass = calloc(1, sizeof(*pass));
    if (!pass) {
        return false;
    }

    pass->context = desc->context;
    pass->pipeline_layout = desc->pipeline_layout;

    VkDevice device = pb_context_device(pass->context);
    if (!pb_rhi_shader_module_from_file(device, desc->vert_spv_path, &pass->vert_module) ||
        !pb_rhi_shader_module_from_file(device, desc->frag_spv_path, &pass->frag_module) ||
        !create_depth_resources(pass) ||
        !create_render_pass(pass) ||
        !create_framebuffer(pass) ||
        !create_pipeline_variant(pass, VK_CULL_MODE_BACK_BIT, &pass->pipeline) ||
        !create_pipeline_variant(pass, VK_CULL_MODE_NONE, &pass->pipeline_double_sided)) {
        pb_shadow_pass_destroy(pass);
        return false;
    }

    *out_pass = pass;
    return true;
}

void pb_shadow_pass_destroy(pb_shadow_pass *pass)
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
        if (pass->pipeline_double_sided) {
            vkDestroyPipeline(device, pass->pipeline_double_sided, NULL);
        }
        if (pass->framebuffer) {
            vkDestroyFramebuffer(device, pass->framebuffer, NULL);
        }
        if (pass->render_pass) {
            vkDestroyRenderPass(device, pass->render_pass, NULL);
        }
        if (pass->sampler) {
            vkDestroySampler(device, pass->sampler, NULL);
        }
        if (pass->depth_view) {
            vkDestroyImageView(device, pass->depth_view, NULL);
        }
        if (pass->depth_image) {
            vkDestroyImage(device, pass->depth_image, NULL);
        }
        if (pass->depth_memory) {
            vkFreeMemory(device, pass->depth_memory, NULL);
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
