/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pbr/point_shadow_pass.h"

#include "core/log.h"
#include "pbr/vertex.h"
#include "pb_context_internal.h"
#include "peaberry/peaberry_gltf.h"
#include "peaberry/peaberry_math.h"
#include "rhi/alloc.h"
#include "rhi/buffer.h"
#include "rhi/ring_buffer.h"
#include "rhi/shader.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PB_POINT_SHADOW_MAP_SIZE 512

/* GPU-side per-face data the vertex/fragment shaders read at binding 15. Kept
 * in a per-frame-slot ring buffer so the cube pass can be re-entrant across
 * frames in flight. */
typedef struct pb_point_shadow_frame_ubo {
    pb_mat4 face_view_proj;
    float light_pos[3];
    float light_range;
} pb_point_shadow_frame_ubo;

struct pb_point_shadow_pass {
    pb_context *context;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkPipeline pipeline_double_sided;
    VkRenderPass render_pass;
    /* One cube depth image per slot. */
    VkImage depth_images[PB_POINT_SHADOW_MAX];
    VkDeviceMemory depth_memory[PB_POINT_SHADOW_MAX];
    VkImageView cube_views[PB_POINT_SHADOW_MAX];      /* VK_IMAGE_VIEW_TYPE_CUBE */
    VkImageView face_views[PB_POINT_SHADOW_MAX][6];   /* VK_IMAGE_VIEW_TYPE_2D, one per face */
    VkFramebuffer framebuffers[PB_POINT_SHADOW_MAX][6]; /* one per slot per face */
    VkSampler sampler;
    VkFormat depth_format;
    VkShaderModule vert_module;
    VkShaderModule frag_module;
    /* Per-face UBO updated for each face draw. */
    pb_rhi_ring_buffer frame_ubo;
    uint32_t frame_slot;
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

static bool create_cube_resources(pb_point_shadow_pass *pass)
{
    VkDevice device = pb_context_device(pass->context);

    for (uint32_t slot = 0; slot < PB_POINT_SHADOW_MAX; ++slot) {
        VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = pass->depth_format,
            .extent = { PB_POINT_SHADOW_MAP_SIZE, PB_POINT_SHADOW_MAP_SIZE, 1 },
            .mipLevels = 1,
            .arrayLayers = 6,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        if (vkCreateImage(device, &image_info, NULL, &pass->depth_images[slot]) != VK_SUCCESS) {
            return false;
        }

        VkMemoryRequirements mem_reqs;
        vkGetImageMemoryRequirements(device, pass->depth_images[slot], &mem_reqs);

        const pb_vk_context *vk = &pass->context->vk;
        const uint32_t mem_type = pb_rhi_find_memory_type(
            vk, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mem_type == UINT32_MAX) {
            return false;
        }

        VkMemoryAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mem_reqs.size,
            .memoryTypeIndex = mem_type,
        };
        if (vkAllocateMemory(device, &alloc_info, NULL, &pass->depth_memory[slot]) != VK_SUCCESS) {
            return false;
        }
        if (vkBindImageMemory(device, pass->depth_images[slot], pass->depth_memory[slot], 0) != VK_SUCCESS) {
            return false;
        }

        /* Cube view (sampled by the forward pass as samplerCubeShadow). */
        VkImageViewCreateInfo cube_view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = pass->depth_images[slot],
            .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
            .format = pass->depth_format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .levelCount = 1,
                .layerCount = 6,
            },
        };
        if (vkCreateImageView(device, &cube_view_info, NULL, &pass->cube_views[slot]) != VK_SUCCESS) {
            return false;
        }

        /* Six per-face 2D views (one per face for framebuffer attachment). */
        for (uint32_t face = 0; face < 6; ++face) {
            VkImageViewCreateInfo face_view_info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = pass->depth_images[slot],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = pass->depth_format,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = face,
                    .layerCount = 1,
                },
            };
            if (vkCreateImageView(device, &face_view_info, NULL, &pass->face_views[slot][face]) != VK_SUCCESS) {
                return false;
            }

            VkFramebufferCreateInfo fb_info = {
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = pass->render_pass,
                .attachmentCount = 1,
                .pAttachments = &pass->face_views[slot][face],
                .width = PB_POINT_SHADOW_MAP_SIZE,
                .height = PB_POINT_SHADOW_MAP_SIZE,
                .layers = 1,
            };
            if (vkCreateFramebuffer(device, &fb_info, NULL, &pass->framebuffers[slot][face]) != VK_SUCCESS) {
                return false;
            }
        }
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

static bool create_render_pass(pb_point_shadow_pass *pass)
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

    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &depth_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dep,
    };

    return vkCreateRenderPass(pb_context_device(pass->context), &info, NULL, &pass->render_pass) == VK_SUCCESS;
}

static bool create_pipeline_variant(
    pb_point_shadow_pass *pass,
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
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
          .offset = offsetof(pb_pbr_vertex, pos) },
        { .location = 4, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
          .offset = offsetof(pb_pbr_vertex, joints) },
        { .location = 5, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
          .offset = offsetof(pb_pbr_vertex, weights) },
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
        .cullMode = cull_mode,
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
        /* LESS_OR_EQUAL so equal-depth writes (from gl_FragDepth) don't fight
         * the automatic interpolated depth. */
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

/* Build the 6 view-projection matrices for a point-light shadow cube. Standard
 * Vulkan cube face orientation: +X, -X, +Y, -Y, +Z, -Z with the conventional
 * up vectors. 90-degree FOV perspective, near 0.1, far = range. */
static void build_face_view_proj(
    const float light_pos[3],
    float range,
    uint32_t face,
    pb_mat4 out_vp)
{
    static const float targets[6][3] = {
        { 1.0f,  0.0f,  0.0f},  /* +X */
        {-1.0f,  0.0f,  0.0f},  /* -X */
        { 0.0f,  1.0f,  0.0f},  /* +Y */
        { 0.0f, -1.0f,  0.0f},  /* -Y */
        { 0.0f,  0.0f,  1.0f},  /* +Z */
        { 0.0f,  0.0f, -1.0f},  /* -Z */
    };
    /* Vulkan cube conventions (see VK_REMAINING_ARRAY_LAYERS / spec table 26).
     * Up vectors chosen so each face's image-space +Y points up consistently. */
    static const float ups[6][3] = {
        {0.0f, -1.0f,  0.0f},  /* +X */
        {0.0f, -1.0f,  0.0f},  /* -X */
        {0.0f,  0.0f,  1.0f},  /* +Y */
        {0.0f,  0.0f, -1.0f},  /* -Y */
        {0.0f, -1.0f,  0.0f},  /* +Z */
        {0.0f, -1.0f,  0.0f},  /* -Z */
    };

    const float eye[3] = { light_pos[0], light_pos[1], light_pos[2] };
    const float target[3] = {
        light_pos[0] + targets[face][0],
        light_pos[1] + targets[face][1],
        light_pos[2] + targets[face][2],
    };

    pb_mat4 view;
    pb_mat4 proj;
    pb_mat4_look_at(view, eye, target, ups[face]);
    pb_mat4_perspective(proj, pb_radians(90.0f), 1.0f, 0.1f, range);

    pb_mat4_mul(proj, view, out_vp);
}

static void record_face_draw(
    pb_point_shadow_pass *pass,
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
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(push),
        &push);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &offset);
    vkCmdBindIndexBuffer(cmd, index_buffer, 0, draw->mesh.index_type);
    vkCmdDrawIndexed(cmd, draw->mesh.index_count, instance_count, 0, 0, 0);
}

void pb_point_shadow_pass_record(
    pb_point_shadow_pass *pass,
    VkCommandBuffer cmd,
    const pb_gltf_scene *scene,
    uint32_t slot,
    const float light_pos[3],
    float light_range,
    VkDescriptorSet *material_descriptor_sets,
    uint32_t descriptor_set_count,
    const uint32_t *dynamic_offsets,
    uint32_t dynamic_offset_count,
    uint32_t instanced_draw_index,
    uint32_t instanced_count)
{
    if (!pass || !scene || !cmd || !material_descriptor_sets || descriptor_set_count == 0 ||
        slot >= PB_POINT_SHADOW_MAX) {
        return;
    }

    const float range = light_range > 0.1f ? light_range : 50.0f;
    const bool has_instanced = instanced_count > 0 && instanced_draw_index < scene->draw_count;

    const VkExtent2D extent = { PB_POINT_SHADOW_MAP_SIZE, PB_POINT_SHADOW_MAP_SIZE };
    VkClearValue clear = { .depthStencil = { 1.0f, 0 } };

    for (uint32_t face = 0; face < 6; ++face) {
        /* Update the per-face UBO (binding 15) with this face's view-proj. */
        pb_point_shadow_frame_ubo frame_ubo;
        build_face_view_proj(light_pos, range, face, frame_ubo.face_view_proj);
        memcpy(frame_ubo.light_pos, light_pos, sizeof(frame_ubo.light_pos));
        frame_ubo.light_range = range;
        pb_rhi_ring_buffer_write_slot(&pass->frame_ubo, pass->frame_slot, &frame_ubo, sizeof(frame_ubo));

        /* The base dynamic offsets from the caller cover binding 0 (frame UBO)
         * and binding 13 (light list). Append the point-shadow UBO's offset
         * (binding 15) as the last entry. */
        uint32_t offsets[4];
        const uint32_t copy_n = dynamic_offset_count <= 3 ? dynamic_offset_count : 3;
        memcpy(offsets, dynamic_offsets, copy_n * sizeof(uint32_t));
        offsets[copy_n] = (uint32_t)pb_rhi_ring_buffer_slot_offset(&pass->frame_ubo, pass->frame_slot);

        VkRenderPassBeginInfo rp_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = pass->render_pass,
            .framebuffer = pass->framebuffers[slot][face],
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
            const VkPipeline pipeline = scene->materials[draw->material_index].double_sided
                ? pass->pipeline_double_sided
                : pass->pipeline;
            record_face_draw(
                pass,
                cmd,
                scene,
                d,
                draw,
                pipeline,
                material_descriptor_sets[draw->material_index],
                offsets,
                copy_n + 1,
                1);
        }

        if (has_instanced) {
            const pb_gltf_draw *draw = &scene->draws[instanced_draw_index];
            if (draw->material_index < descriptor_set_count) {
                const VkPipeline pipeline = scene->materials[draw->material_index].double_sided
                    ? pass->pipeline_double_sided
                    : pass->pipeline;
                record_face_draw(
                    pass,
                    cmd,
                    scene,
                    instanced_draw_index,
                    draw,
                    pipeline,
                    material_descriptor_sets[draw->material_index],
                    offsets,
                    copy_n + 1,
                    instanced_count);
            }
        }

        vkCmdEndRenderPass(cmd);
    }
}

const VkImageView *pb_point_shadow_pass_views(const pb_point_shadow_pass *pass)
{
    return pass ? pass->cube_views : NULL;
}

VkSampler pb_point_shadow_pass_sampler(const pb_point_shadow_pass *pass)
{
    return pass ? pass->sampler : VK_NULL_HANDLE;
}

VkBuffer pb_point_shadow_pass_frame_buffer(const pb_point_shadow_pass *pass)
{
    return pass ? pb_rhi_ring_buffer_handle(&pass->frame_ubo) : VK_NULL_HANDLE;
}

VkDeviceSize pb_point_shadow_pass_frame_slot_offset(const pb_point_shadow_pass *pass)
{
    return pass ? pb_rhi_ring_buffer_slot_offset(&pass->frame_ubo, pass->frame_slot) : 0;
}

uint32_t pb_point_shadow_pass_frame_ubo_size(void)
{
    return (uint32_t)sizeof(pb_point_shadow_frame_ubo);
}

void pb_point_shadow_pass_set_frame_slot(pb_point_shadow_pass *pass, uint32_t slot)
{
    if (pass) {
        pass->frame_slot = slot;
    }
}

bool pb_point_shadow_pass_create(const pb_point_shadow_pass_desc *desc, pb_point_shadow_pass **out_pass)
{
    if (!desc || !desc->context || !desc->pipeline_layout || !desc->vert_spv_path || !desc->frag_spv_path || !out_pass) {
        return false;
    }

    pb_point_shadow_pass *pass = calloc(1, sizeof(*pass));
    if (!pass) {
        return false;
    }
    pass->context = desc->context;
    pass->pipeline_layout = desc->pipeline_layout;
    pass->frame_slot = 0;

    VkDevice device = pb_context_device(desc->context);

    if (!pb_rhi_shader_module_from_file(device, desc->vert_spv_path, &pass->vert_module) ||
        !pb_rhi_shader_module_from_file(device, desc->frag_spv_path, &pass->frag_module) ||
        !choose_depth_format(pb_context_physical_device(desc->context), &pass->depth_format) ||
        !create_render_pass(pass) ||
        !create_cube_resources(pass) ||
        !create_pipeline_variant(pass, VK_CULL_MODE_BACK_BIT, &pass->pipeline) ||
        !create_pipeline_variant(pass, VK_CULL_MODE_NONE, &pass->pipeline_double_sided) ||
        !pb_rhi_ring_buffer_create(
            desc->context,
            sizeof(pb_point_shadow_frame_ubo),
            PB_FRAMES_IN_FLIGHT,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            pb_rhi_min_uniform_buffer_offset_alignment(desc->context),
            &pass->frame_ubo)) {
        pb_point_shadow_pass_destroy(pass);
        return false;
    }

    *out_pass = pass;
    return true;
}

void pb_point_shadow_pass_destroy(pb_point_shadow_pass *pass)
{
    if (!pass) {
        return;
    }

    if (pass->context && pb_context_device_ready(pass->context)) {
        VkDevice device = pb_context_device(pass->context);
        pb_context_wait_device_idle(pass->context);

        if (pass->pipeline) {
            vkDestroyPipeline(device, pass->pipeline, NULL);
        }
        if (pass->pipeline_double_sided) {
            vkDestroyPipeline(device, pass->pipeline_double_sided, NULL);
        }
        pb_rhi_ring_buffer_destroy(pass->context, &pass->frame_ubo);
        if (pass->sampler) {
            vkDestroySampler(device, pass->sampler, NULL);
        }
        for (uint32_t slot = 0; slot < PB_POINT_SHADOW_MAX; ++slot) {
            for (uint32_t face = 0; face < 6; ++face) {
                if (pass->framebuffers[slot][face]) {
                    vkDestroyFramebuffer(device, pass->framebuffers[slot][face], NULL);
                }
                if (pass->face_views[slot][face]) {
                    vkDestroyImageView(device, pass->face_views[slot][face], NULL);
                }
            }
            if (pass->cube_views[slot]) {
                vkDestroyImageView(device, pass->cube_views[slot], NULL);
            }
            if (pass->depth_images[slot]) {
                vkDestroyImage(device, pass->depth_images[slot], NULL);
            }
            if (pass->depth_memory[slot]) {
                vkFreeMemory(device, pass->depth_memory[slot], NULL);
            }
        }
        if (pass->render_pass) {
            vkDestroyRenderPass(device, pass->render_pass, NULL);
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
