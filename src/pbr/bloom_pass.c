/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/* Phase 15.3 — bloom via a downsample/upsample mip chain.
 *
 * record() runs each frame, after the forward pass writes the HDR scene color:
 *
 *   1. Downsample chain: HDR (mip 0) → bloom mip 1 → mip 2 → ... → mip N.
 *      Each step applies a Karis soft-threshold bright-pass + 5-tap tent
 *      downsample. Only HDR-above-threshold pixels contribute strongly.
 *   2. Upsample chain: mip N → mip N-1 → ... → mip 0. Each step 9-tap
 *      tent-filters the smaller mip and blends additively (spread factor)
 *      into the next-larger mip, spreading the bright signal outward.
 *
 * Mip 0 of the bloom texture holds the final bloom result; the tonemap post
 * pass composites scene HDR + bloom before ACES.
 *
 * The bloom texture lives in VK_IMAGE_LAYOUT_GENERAL (storage images need it
 * for read+write). Per-mip image barriers serialize the chain. */

#include "bloom_pass.h"

#include "core/log.h"
#include "pb_context_internal.h"
#include "peaberry/peaberry_gltf.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/alloc.h"
#include "rhi/shader.h"
#include "rhi/texture.h"

#include <stdlib.h>

#define PB_BLOOM_WORKGROUP 16u
#define PB_BLOOM_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
#define PB_BLOOM_MAX_MIPS 8u

/* Push constants — shared by both shaders (they read the fields they need). */
typedef struct pb_bloom_push {
    float texel_size_x;
    float texel_size_y;
    float threshold;   /* downsample only; upsample ignores */
    float spread;      /* upsample only; downsample ignores */
    uint32_t source_mip;
} pb_bloom_push;

typedef struct pb_pbr_bloom_pass {
    pb_context *context;

    VkShaderModule downsample_module;
    VkShaderModule upsample_module;

    VkPipeline downsample_pipeline;
    VkPipeline upsample_pipeline;
    VkPipelineLayout pipeline_layout;
    VkDescriptorSetLayout set_layout;

    /* One descriptor set per chain hop, per frame in flight. Updating a set
     * while a previous frame's command buffer still references it is illegal
     * (VUID-vkUpdateDescriptorSets-None-03047). We double-buffer: [frame_slot]
     * [hop]. Max hops each way: mip_count-1. */
    VkDescriptorPool pool;
    VkDescriptorSet downsample_sets[PB_FRAMES_IN_FLIGHT][PB_BLOOM_MAX_MIPS - 1];
    VkDescriptorSet upsample_sets[PB_FRAMES_IN_FLIGHT][PB_BLOOM_MAX_MIPS - 1];
    uint32_t frame_slot;

    /* The bloom mip pyramid. */
    pb_rhi_texture bloom;
    VkImageView mip_views[PB_BLOOM_MAX_MIPS];  /* per-mip storage-image views */
    VkImageView result_view;                   /* mip-0-only view for tonemap read */

    /* A sampler for reading the source (HDR or smaller bloom mip) via
     * combined-image-sampler. The bloom texture's own sampler (linear,
     * clamp-to-edge, maxLod = mip_count) works for both. */
    uint32_t mip_count;
    uint32_t width;     /* full-res dimensions (mip 0) */
    uint32_t height;

    /* Tunables. */
    float threshold;
    float spread;
} pb_pbr_bloom_pass;

/* ----------------------------------------------------------------------- */

static bool create_bloom_texture(pb_pbr_bloom_pass *pass, uint32_t width, uint32_t height)
{
    VkDevice device = pb_context_device(pass->context);

    /* The bloom pyramid needs STORAGE (compute read+write) + SAMPLED (the
     * final mip 0 is read by tonemap.frag as a combined image sampler) +
     * TRANSFER_DST (initial clear to GENERAL-zero). */
    if (!pb_rhi_texture_create_2d(
            pass->context,
            width,
            height,
            pass->mip_count,
            PB_BLOOM_FORMAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            &pass->bloom)) {
        return false;
    }

    /* Per-mip storage-image views. The pb_rhi_texture view covers all mips
     * (good for tonemap's combined-image-sampler read of mip 0), but compute
     * imageStore needs a view scoped to a single mip level. */
    for (uint32_t m = 0; m < pass->mip_count; ++m) {
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = pass->bloom.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = PB_BLOOM_FORMAT,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = m,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        if (vkCreateImageView(device, &view_info, NULL, &pass->mip_views[m]) != VK_SUCCESS) {
            return false;
        }
    }

    /* A mip-0-only view for the tonemap post pass to read the final bloom
     * result. Using a dedicated mip-0 view (rather than the all-mips
     * pb_rhi_texture view) avoids the sampler spanning mixed layouts (mip 0
     * ends in SHADER_READ_ONLY_OPTIMAL, higher mips stay in GENERAL). */
    VkImageViewCreateInfo result_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = pass->bloom.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = PB_BLOOM_FORMAT,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    if (vkCreateImageView(device, &result_info, NULL, &pass->result_view) != VK_SUCCESS) {
        return false;
    }

    return true;
}

static bool create_descriptor_set_layout(VkDevice device, VkDescriptorSetLayout *out)
{
    VkDescriptorSetLayoutBinding bindings[] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = sizeof(bindings) / sizeof(bindings[0]),
        .pBindings = bindings,
    };

    return vkCreateDescriptorSetLayout(device, &info, NULL, out) == VK_SUCCESS;
}

static bool create_pipeline_layout(VkDevice device, VkDescriptorSetLayout set_layout, VkPipelineLayout *out)
{
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(pb_bloom_push),
    };

    VkPipelineLayoutCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };

    return vkCreatePipelineLayout(device, &info, NULL, out) == VK_SUCCESS;
}

static bool create_compute_pipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule module, VkPipeline *out)
{
    VkPipelineShaderStageCreateInfo stage = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = module,
        .pName = "main",
    };

    VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = stage,
        .layout = layout,
    };

    return vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, NULL, out) == VK_SUCCESS;
}

/* Allocate one set per downsample hop and one per upsample hop, per frame in
 * flight. The per-frame-slot sets avoid the descriptor-set-in-use hazard
 * (VUID-vkUpdateDescriptorSets-None-03047) when 2 frames overlap. */
static bool allocate_descriptor_sets(
    VkDevice device,
    VkDescriptorSetLayout layout,
    uint32_t hop_count,
    VkDescriptorPool *out_pool,
    VkDescriptorSet downsample_sets[PB_FRAMES_IN_FLIGHT][PB_BLOOM_MAX_MIPS - 1],
    VkDescriptorSet upsample_sets[PB_FRAMES_IN_FLIGHT][PB_BLOOM_MAX_MIPS - 1])
{
    const uint32_t sets_per_frame = hop_count * 2u;
    const uint32_t total_sets = sets_per_frame * PB_FRAMES_IN_FLIGHT;
    VkDescriptorPoolSize sizes[] = {
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = total_sets },
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = total_sets },
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = total_sets,
        .poolSizeCount = sizeof(sizes) / sizeof(sizes[0]),
        .pPoolSizes = sizes,
    };

    if (vkCreateDescriptorPool(device, &pool_info, NULL, out_pool) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetLayout layouts[PB_FRAMES_IN_FLIGHT * PB_BLOOM_MAX_MIPS * 2u];
    VkDescriptorSet all_sets[PB_FRAMES_IN_FLIGHT * PB_BLOOM_MAX_MIPS * 2u];
    for (uint32_t i = 0; i < total_sets; ++i) {
        layouts[i] = layout;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = *out_pool,
        .descriptorSetCount = total_sets,
        .pSetLayouts = layouts,
    };
    if (vkAllocateDescriptorSets(device, &alloc_info, all_sets) != VK_SUCCESS) {
        return false;
    }
    for (uint32_t f = 0; f < PB_FRAMES_IN_FLIGHT; ++f) {
        for (uint32_t i = 0; i < hop_count; ++i) {
            const uint32_t base = f * sets_per_frame;
            downsample_sets[f][i] = all_sets[base + i];
            upsample_sets[f][i] = all_sets[base + hop_count + i];
        }
    }
    return true;
}

/* ----------------------------------------------------------------------- */

pb_pbr_bloom_pass *pb_pbr_bloom_pass_create(const pb_pbr_bloom_pass_desc *desc)
{
    if (!desc || !desc->context || !desc->downsample_spv_path || !desc->upsample_spv_path) {
        pb_log_error("pb_pbr_bloom_pass_create: missing required desc fields");
        return NULL;
    }

    pb_pbr_bloom_pass *pass = calloc(1, sizeof(*pass));
    if (!pass) {
        return NULL;
    }
    pass->context = desc->context;
    pass->mip_count = desc->mip_count > 0 && desc->mip_count <= PB_BLOOM_MAX_MIPS ? desc->mip_count : 6;
    pass->threshold = desc->threshold > 0.0f ? desc->threshold : 1.0f;
    pass->spread = desc->spread > 0.0f ? desc->spread : 0.65f;

    /* The pyramid dimensions aren't known until the first record() (the caller
     * passes the HDR extent). Defer texture creation + per-mip views to the
     * first record, or create lazily when extent is known. For now, create at
     * a default size; record() will recreate if the extent differs. */
    pass->width = 0;
    pass->height = 0;

    VkDevice device = pb_context_device(desc->context);

    const uint32_t hop_count = pass->mip_count > 1 ? pass->mip_count - 1 : 1;
    if (!pb_rhi_shader_module_from_file(device, desc->downsample_spv_path, &pass->downsample_module) ||
        !pb_rhi_shader_module_from_file(device, desc->upsample_spv_path, &pass->upsample_module) ||
        !create_descriptor_set_layout(device, &pass->set_layout) ||
        !create_pipeline_layout(device, pass->set_layout, &pass->pipeline_layout) ||
        !create_compute_pipeline(device, pass->pipeline_layout, pass->downsample_module, &pass->downsample_pipeline) ||
        !create_compute_pipeline(device, pass->pipeline_layout, pass->upsample_module, &pass->upsample_pipeline) ||
        !allocate_descriptor_sets(
            device,
            pass->set_layout,
            hop_count,
            &pass->pool,
            pass->downsample_sets,
            pass->upsample_sets)) {
        pb_pbr_bloom_pass_destroy(pass);
        return NULL;
    }

    return pass;
}

void pb_pbr_bloom_pass_destroy(pb_pbr_bloom_pass *pass)
{
    if (!pass) {
        return;
    }

    VkDevice device = pass->context ? pb_context_device(pass->context) : VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE && pb_context_device_ready(pass->context)) {
        pb_context_wait_device_idle(pass->context);

        if (pass->downsample_pipeline) {
            vkDestroyPipeline(device, pass->downsample_pipeline, NULL);
        }
        if (pass->upsample_pipeline) {
            vkDestroyPipeline(device, pass->upsample_pipeline, NULL);
        }
        if (pass->pipeline_layout) {
            vkDestroyPipelineLayout(device, pass->pipeline_layout, NULL);
        }
        if (pass->set_layout) {
            vkDestroyDescriptorSetLayout(device, pass->set_layout, NULL);
        }
        if (pass->pool) {
            vkDestroyDescriptorPool(device, pass->pool, NULL);
        }
        for (uint32_t m = 0; m < pass->mip_count && m < PB_BLOOM_MAX_MIPS; ++m) {
            if (pass->mip_views[m]) {
                vkDestroyImageView(device, pass->mip_views[m], NULL);
            }
        }
        if (pass->result_view) {
            vkDestroyImageView(device, pass->result_view, NULL);
        }
        pb_rhi_texture_destroy(pass->context, &pass->bloom);
        if (pass->downsample_module) {
            vkDestroyShaderModule(device, pass->downsample_module, NULL);
        }
        if (pass->upsample_module) {
            vkDestroyShaderModule(device, pass->upsample_module, NULL);
        }
    }
    free(pass);
}

/* Ensure the bloom pyramid matches the requested extent, (re)creating the
 * texture + per-mip views if the extent changed or this is the first call. */
static bool ensure_bloom_texture(pb_pbr_bloom_pass *pass, VkExtent2D extent)
{
    if (pass->width == extent.width && pass->height == extent.height && pass->bloom.image != VK_NULL_HANDLE) {
        return true;
    }

    VkDevice device = pb_context_device(pass->context);

    /* Destroy old views + texture if resizing. */
    for (uint32_t m = 0; m < pass->mip_count && m < PB_BLOOM_MAX_MIPS; ++m) {
        if (pass->mip_views[m]) {
            vkDestroyImageView(device, pass->mip_views[m], NULL);
            pass->mip_views[m] = VK_NULL_HANDLE;
        }
    }
    if (pass->result_view) {
        vkDestroyImageView(device, pass->result_view, NULL);
        pass->result_view = VK_NULL_HANDLE;
    }
    if (pass->bloom.image != VK_NULL_HANDLE) {
        pb_rhi_texture_destroy(pass->context, &pass->bloom);
    }

    pass->width = extent.width;
    pass->height = extent.height;
    if (!create_bloom_texture(pass, extent.width, extent.height)) {
        pass->width = 0;
        pass->height = 0;
        return false;
    }
    return true;
}

/* Transition a single mip level between layouts. */
static void barrier_mip(
    VkCommandBuffer cmd,
    VkImage image,
    uint32_t mip,
    VkImageLayout old_layout,
    VkImageLayout new_layout,
    VkAccessFlags src_access,
    VkAccessFlags dst_access)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = mip,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0, NULL,
        0, NULL,
        1, &barrier);
}

static void write_image_descriptors(
    pb_pbr_bloom_pass *pass,
    VkDescriptorSet set,
    VkImageView source_view,
    VkSampler source_sampler,
    uint32_t source_mip_for_sampler,
    VkImageView dest_view)
{
    /* source_mip_for_sampler is unused for storage-image sources; we always
     * bind a per-mip view as a combined-image-sampler so the shader can
     * textureLod from the right level. But a per-mip view already restricts
     * the visible mip, so the LOD passed to textureLod should be 0 relative
     * to the view. The shader passes source_mip; with per-mip views we set
     * it to 0 in the push constant at dispatch time. */
    (void)source_mip_for_sampler;

    VkDevice device = pb_context_device(pass->context);

    VkDescriptorImageInfo src_info = {
        .sampler = source_sampler,
        .imageView = source_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkDescriptorImageInfo dst_info = {
        .imageView = dest_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };

    VkWriteDescriptorSet writes[] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &src_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .pImageInfo = &dst_info,
        },
    };
    vkUpdateDescriptorSets(device, sizeof(writes) / sizeof(writes[0]), writes, 0, NULL);
}

void pb_pbr_bloom_pass_record(
    pb_pbr_bloom_pass *pass,
    VkCommandBuffer cmd,
    VkImageView hdr_scene_view,
    VkSampler hdr_scene_sampler,
    VkExtent2D extent)
{
    if (!pass || !pass->downsample_pipeline || hdr_scene_view == VK_NULL_HANDLE) {
        return;
    }

    if (!ensure_bloom_texture(pass, extent)) {
        return;
    }

    /* Transition the whole bloom pyramid UNDEFINED -> GENERAL so the compute
     * shaders can read/write it. */
    {
        VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = pass->bloom.image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = pass->mip_count,
                .layerCount = 1,
            },
        };
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0, NULL,
            0, NULL,
            1, &barrier);
    }

    /* --- Downsample chain: HDR (mip 0) -> bloom mip 1 -> ... -> mip N-1 --- */
    for (uint32_t m = 0; m + 1 < pass->mip_count; ++m) {
        const uint32_t src_mip = m;
        const uint32_t dst_mip = m + 1;
        const uint32_t src_w = pass->width >> src_mip;
        const uint32_t src_h = pass->height >> src_mip;
        VkDescriptorSet set = pass->downsample_sets[pass->frame_slot][m];

        /* Source binding: mip 0 reads the HDR scene; higher mips read the
         * previous bloom mip (via its per-mip view). */
        if (src_mip == 0) {
            /* HDR scene color — it's in SHADER_READ_ONLY_OPTIMAL (the caller
             * transitioned it). Bind it as the combined-image-sampler source. */
            VkDescriptorImageInfo src_info = {
                .sampler = hdr_scene_sampler,
                .imageView = hdr_scene_view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
            VkDescriptorImageInfo dst_info = {
                .imageView = pass->mip_views[dst_mip],
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            };
            VkWriteDescriptorSet writes[] = {
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = set,
                    .dstBinding = 0,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .descriptorCount = 1,
                    .pImageInfo = &src_info,
                },
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = set,
                    .dstBinding = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    .descriptorCount = 1,
                    .pImageInfo = &dst_info,
                },
            };
            vkUpdateDescriptorSets(pb_context_device(pass->context), 2, writes, 0, NULL);
        } else {
            write_image_descriptors(pass, set,
                pass->mip_views[src_mip], pass->bloom.sampler, 0,
                pass->mip_views[dst_mip]);
        }

        /* Wait for the previous write to src_mip before reading it. */
        if (src_mip > 0) {
            barrier_mip(cmd, pass->bloom.image, src_mip,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        }

        pb_bloom_push push = {
            .texel_size_x = 1.0f / (float)src_w,
            .texel_size_y = 1.0f / (float)src_h,
            /* Bright-pass only on HDR→mip1; later hops keep threshold 0 so
             * soft_threshold is nearly identity on already-filtered bloom. */
            .threshold = (src_mip == 0) ? pass->threshold : 0.0f,
            .spread = pass->spread,
            .source_mip = 0u, /* per-mip (or single-mip HDR) view → LOD 0 */
        };

        const uint32_t dst_w = pass->width >> dst_mip;
        const uint32_t dst_h = pass->height >> dst_mip;
        const uint32_t groups_x = (dst_w + PB_BLOOM_WORKGROUP - 1) / PB_BLOOM_WORKGROUP;
        const uint32_t groups_y = (dst_h + PB_BLOOM_WORKGROUP - 1) / PB_BLOOM_WORKGROUP;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass->downsample_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass->pipeline_layout, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(cmd, pass->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, groups_x, groups_y, 1);

        /* Block writes to dst_mip until this dispatch finishes. */
        barrier_mip(cmd, pass->bloom.image, dst_mip,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    }

    /* --- Upsample chain: mip N-1 -> mip N-2 -> ... -> mip 0 --- */
    for (uint32_t dst_mip = pass->mip_count - 1; dst_mip > 0; --dst_mip) {
        const uint32_t src_mip = dst_mip;
        const uint32_t src_w = pass->width >> src_mip;
        const uint32_t src_h = pass->height >> src_mip;
        /* Map dst_mip N-1..1 → set index 0..N-2 */
        const uint32_t hop = (pass->mip_count - 1) - dst_mip;
        VkDescriptorSet set = pass->upsample_sets[pass->frame_slot][hop];

        write_image_descriptors(pass, set,
            pass->mip_views[src_mip], pass->bloom.sampler, 0,
            pass->mip_views[dst_mip - 1]);

        pb_bloom_push push = {
            .texel_size_x = 1.0f / (float)src_w,
            .texel_size_y = 1.0f / (float)src_h,
            .threshold = pass->threshold,
            .spread = pass->spread,
            .source_mip = 0u, /* per-mip view → LOD 0 */
        };

        const uint32_t dw = pass->width >> (dst_mip - 1);
        const uint32_t dh = pass->height >> (dst_mip - 1);
        const uint32_t groups_x = (dw + PB_BLOOM_WORKGROUP - 1) / PB_BLOOM_WORKGROUP;
        const uint32_t groups_y = (dh + PB_BLOOM_WORKGROUP - 1) / PB_BLOOM_WORKGROUP;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass->upsample_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass->pipeline_layout, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(cmd, pass->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, groups_x, groups_y, 1);

        /* Block writes to dst_mip-1 before the next iteration reads it. */
        barrier_mip(cmd, pass->bloom.image, dst_mip - 1,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    }

    /* Transition mip 0 (the final bloom result) GENERAL -> SHADER_READ_ONLY
     * so the tonemap post pass can sample it as a combined-image-sampler. */
    {
        VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = pass->bloom.image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0,
            0, NULL,
            0, NULL,
            1, &barrier);
    }

    /* Rotate the frame slot so the next record() writes to a different set of
     * descriptor sets (avoids the in-use hazard with 2 frames in flight). */
    pass->frame_slot = (pass->frame_slot + 1) % PB_FRAMES_IN_FLIGHT;
}

VkImageView pb_pbr_bloom_pass_result_view(const pb_pbr_bloom_pass *pass)
{
    /* The post pass reads mip 0 via the bloom texture's full view (covers all
     * mips; tonemap samples LOD 0). */
    return pass ? pass->result_view : VK_NULL_HANDLE;
}

VkSampler pb_pbr_bloom_pass_sampler(const pb_pbr_bloom_pass *pass)
{
    return pass ? pass->bloom.sampler : VK_NULL_HANDLE;
}
