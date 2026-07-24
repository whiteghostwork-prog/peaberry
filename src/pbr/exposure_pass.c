/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/* Phase 15.2 — auto-exposure via a luminance histogram + temporal adaptation.
 *
 * Pipeline (recorded once per frame by pb_pbr_exposure_pass_record):
 *
 *   1. vkCmdFillBuffer(0) on the histogram SSBO  (clear)
 *   2. dispatch histogram.comp  — one 16x16 WG per tile of HDR pixels;
 *      each thread bins its pixel's log-luminance into a shared-memory
 *      histogram, then flushes it to the SSBO with integer atomicAdd.
 *   3. buffer barrier (COMPUTE -> COMPUTE) on the histogram SSBO
 *   4. dispatch average.comp   — one WG of 256 threads; tree-reduces the
 *      256-bin histogram into a weighted log-average luminance, derives
 *      target exposure = key / avgLum, lerps the previous exposure toward
 *      it (eye adaptation), and writes the result to the exposure UBO.
 *
 * The exposure UBO is persistently mapped + host coherent: the post pass binds
 * it as tonemap.frag binding 1, and the host can read it for debugging/tests
 * (pb_pbr_exposure_pass_current) once the GPU work has completed. */

#include "exposure_pass.h"

#include "core/log.h"
#include "pb_context_internal.h"
#include "peaberry/peaberry_gltf.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/alloc.h"
#include "rhi/buffer.h"
#include "rhi/shader.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define PB_EXPOSURE_HISTOGRAM_BINS 256u
#define PB_EXPOSURE_HISTOGRAM_BYTES (PB_EXPOSURE_HISTOGRAM_BINS * sizeof(uint32_t))
/* std140 vec4 minimum for a uniform block; the shader only reads the float,
 * but the block size must be a multiple of 16. */
#define PB_EXPOSURE_UBO_BYTES 16u
/* histogram.comp workgroup is 16x16 = 256 threads. */
#define PB_EXPOSURE_WG_X 16u
#define PB_EXPOSURE_WG_Y 16u

/* Push-constant parameters shared by both shaders — order matches the GLSL. */
typedef struct pb_exposure_push {
    float min_log_lum;
    float max_log_lum;
    float log_lum_range;
    float key;

    float min_exposure;
    float max_exposure;
    float speed;
    float dt;
} pb_exposure_push;

typedef struct pb_pbr_exposure_pass {
    pb_context *context;

    VkShaderModule histogram_module;
    VkShaderModule average_module;

    VkPipeline histogram_pipeline;
    VkPipeline average_pipeline;
    VkPipelineLayout histogram_pipeline_layout;
    VkPipelineLayout average_pipeline_layout;
    /* The two shaders share binding 0 (histogram SSBO) but differ on binding 1
     * (HDR sampler vs exposure UBO), so they need separate set layouts. */
    VkDescriptorSetLayout histogram_set_layout;
    VkDescriptorSetLayout average_set_layout;

    VkDescriptorPool histogram_pool;
    VkDescriptorSet histogram_set;   /* hdr sampler + histogram SSBO */

    VkDescriptorPool average_pool;
    VkDescriptorSet average_set;     /* histogram SSBO (ro) + exposure UBO */

    pb_rhi_buffer histogram_ssbo;    /* 256 uint, GPU-only              */
    pb_rhi_buffer exposure_ubo;      /* 16 bytes, persistently mapped   */

    VkSampler hdr_sampler_owned;     /* created iff caller passes NULL  */
    VkImageView bound_view;          /* cache: skip redundant writes    */
    VkSampler bound_sampler;
    bool descriptor_written;

    /* Adaptation parameters (mirrored into push constants each frame). */
    pb_exposure_push params;
} pb_pbr_exposure_pass;

/* ----------------------------------------------------------------------- */

static bool create_storage_buffer(
    pb_context *context,
    VkDeviceSize size,
    pb_rhi_memory_usage usage,
    pb_rhi_buffer *out)
{
    pb_rhi_buffer_desc desc = {
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,  /* for vkCmdFillBuffer clear */
        .memory_usage = usage,
    };
    return pb_rhi_buffer_create(context, &desc, out);
}

static bool create_histogram_set_layout(VkDevice device, VkDescriptorSetLayout *out_layout)
{
    /* histogram.comp: binding 0 = histogram SSBO (RW), binding 1 = HDR sampler. */
    VkDescriptorSetLayoutBinding bindings[] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = sizeof(bindings) / sizeof(bindings[0]),
        .pBindings = bindings,
    };

    return vkCreateDescriptorSetLayout(device, &info, NULL, out_layout) == VK_SUCCESS;
}

static bool create_average_set_layout(VkDevice device, VkDescriptorSetLayout *out_layout)
{
    /* average.comp: binding 0 = histogram SSBO (RO), binding 1 = exposure SSBO.
     * Both are storage buffers because average.comp reads the previous frame's
     * exposure and writes the new one (UBOs are read-only in shaders). */
    VkDescriptorSetLayoutBinding bindings[] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = sizeof(bindings) / sizeof(bindings[0]),
        .pBindings = bindings,
    };

    return vkCreateDescriptorSetLayout(device, &info, NULL, out_layout) == VK_SUCCESS;
}

static bool create_pipeline_layout(VkDevice device, VkDescriptorSetLayout set_layout, VkPipelineLayout *out)
{
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(pb_exposure_push),
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

static bool create_compute_pipeline(
    VkDevice device,
    VkPipelineLayout layout,
    VkShaderModule module,
    VkPipeline *out)
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

static bool allocate_descriptor_set(
    VkDevice device,
    VkDescriptorSetLayout layout,
    VkDescriptorPool *out_pool,
    VkDescriptorSet *out_set)
{
    VkDescriptorPoolSize sizes[] = {
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 },
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1 },
        { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1 },
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = sizeof(sizes) / sizeof(sizes[0]),
        .pPoolSizes = sizes,
    };

    if (vkCreateDescriptorPool(device, &pool_info, NULL, out_pool) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = *out_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    };
    return vkAllocateDescriptorSets(device, &alloc_info, out_set) == VK_SUCCESS;
}

static bool create_default_sampler(VkDevice device, VkSampler *out)
{
    VkSamplerCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .minLod = 0.0f,
        .maxLod = 1.0f,
    };
    return vkCreateSampler(device, &info, NULL, out) == VK_SUCCESS;
}

/* ----------------------------------------------------------------------- */

pb_pbr_exposure_pass *pb_pbr_exposure_pass_create(const pb_pbr_exposure_pass_desc *desc)
{
    if (!desc || !desc->context || !desc->histogram_spv_path || !desc->average_spv_path) {
        pb_log_error("pb_pbr_exposure_pass_create: missing required desc fields");
        return NULL;
    }

    pb_pbr_exposure_pass *pass = calloc(1, sizeof(*pass));
    if (!pass) {
        return NULL;
    }
    pass->context = desc->context;

    /* Resolve defaulted parameters. */
    pass->params.min_log_lum = desc->min_log_lum != 0.0f ? desc->min_log_lum : -10.0f;
    pass->params.max_log_lum = desc->max_log_lum != 0.0f ? desc->max_log_lum : 2.0f;
    pass->params.log_lum_range = pass->params.max_log_lum - pass->params.min_log_lum;
    pass->params.key = desc->key > 0.0f ? desc->key : 0.5f;
    pass->params.min_exposure = desc->min_exposure > 0.0f ? desc->min_exposure : 0.1f;
    pass->params.max_exposure = desc->max_exposure > 0.0f ? desc->max_exposure : 2.0f;
    pass->params.speed = desc->speed > 0.0f ? desc->speed : 2.0f;
    pass->params.dt = 0.0f;

    VkDevice device = pb_context_device(desc->context);

    if (!pb_rhi_shader_module_from_file(device, desc->histogram_spv_path, &pass->histogram_module) ||
        !pb_rhi_shader_module_from_file(device, desc->average_spv_path, &pass->average_module) ||
        !create_histogram_set_layout(device, &pass->histogram_set_layout) ||
        !create_average_set_layout(device, &pass->average_set_layout) ||
        !create_pipeline_layout(device, pass->histogram_set_layout, &pass->histogram_pipeline_layout) ||
        !create_pipeline_layout(device, pass->average_set_layout, &pass->average_pipeline_layout) ||
        !create_compute_pipeline(device, pass->histogram_pipeline_layout, pass->histogram_module, &pass->histogram_pipeline) ||
        !create_compute_pipeline(device, pass->average_pipeline_layout, pass->average_module, &pass->average_pipeline) ||
        !create_storage_buffer(pass->context, PB_EXPOSURE_HISTOGRAM_BYTES, PB_RHI_MEMORY_GPU_ONLY, &pass->histogram_ssbo) ||
        !create_storage_buffer(pass->context, PB_EXPOSURE_UBO_BYTES, PB_RHI_MEMORY_CPU_TO_GPU, &pass->exposure_ubo) ||
        !allocate_descriptor_set(device, pass->histogram_set_layout, &pass->histogram_pool, &pass->histogram_set) ||
        !allocate_descriptor_set(device, pass->average_set_layout, &pass->average_pool, &pass->average_set)) {
        pb_pbr_exposure_pass_destroy(pass);
        return NULL;
    }

    /* Seed the exposure UBO with the initial value so the first tonemap pass
     * has a sane value before the first average dispatch completes. */
    float initial = desc->initial_exposure > 0.0f ? desc->initial_exposure : 1.0f;
    if (pass->exposure_ubo.mapped) {
        memcpy(pass->exposure_ubo.mapped, &initial, sizeof(initial));
    } else if (!pb_rhi_buffer_upload(pass->context, &pass->exposure_ubo, &initial, sizeof(initial))) {
        pb_pbr_exposure_pass_destroy(pass);
        return NULL;
    }

    /* Bind the SSBO once at create time — it never changes. The HDR view +
     * sampler for the histogram set, and the exposure UBO for the average set,
     * are written in record() (with caching for the HDR view). */
    VkDescriptorBufferInfo hist_info = {
        .buffer = pass->histogram_ssbo.handle,
        .offset = 0,
        .range = PB_EXPOSURE_HISTOGRAM_BYTES,
    };
    VkWriteDescriptorSet hist_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = pass->histogram_set,
        .dstBinding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .pBufferInfo = &hist_info,
    };
    VkDescriptorBufferInfo hist_ro = hist_info;
    VkWriteDescriptorSet avg_hist_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = pass->average_set,
        .dstBinding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .pBufferInfo = &hist_ro,
    };
    VkDescriptorBufferInfo exp_info = {
        .buffer = pass->exposure_ubo.handle,
        .offset = 0,
        .range = PB_EXPOSURE_UBO_BYTES,
    };
    VkWriteDescriptorSet avg_exp_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = pass->average_set,
        .dstBinding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .pBufferInfo = &exp_info,
    };

    VkWriteDescriptorSet writes[] = { hist_write, avg_hist_write, avg_exp_write };
    vkUpdateDescriptorSets(device, sizeof(writes) / sizeof(writes[0]), writes, 0, NULL);

    return pass;
}

void pb_pbr_exposure_pass_destroy(pb_pbr_exposure_pass *pass)
{
    if (!pass) {
        return;
    }

    VkDevice device = pass->context ? pb_context_device(pass->context) : VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE && pb_context_device_ready(pass->context)) {
        pb_context_wait_device_idle(pass->context);

        if (pass->histogram_pipeline) {
            vkDestroyPipeline(device, pass->histogram_pipeline, NULL);
        }
        if (pass->average_pipeline) {
            vkDestroyPipeline(device, pass->average_pipeline, NULL);
        }
        if (pass->histogram_pipeline_layout) {
            vkDestroyPipelineLayout(device, pass->histogram_pipeline_layout, NULL);
        }
        if (pass->average_pipeline_layout) {
            vkDestroyPipelineLayout(device, pass->average_pipeline_layout, NULL);
        }
        if (pass->histogram_set_layout) {
            vkDestroyDescriptorSetLayout(device, pass->histogram_set_layout, NULL);
        }
        if (pass->average_set_layout) {
            vkDestroyDescriptorSetLayout(device, pass->average_set_layout, NULL);
        }
        if (pass->histogram_pool) {
            vkDestroyDescriptorPool(device, pass->histogram_pool, NULL);
        }
        if (pass->average_pool) {
            vkDestroyDescriptorPool(device, pass->average_pool, NULL);
        }
        if (pass->hdr_sampler_owned) {
            vkDestroySampler(device, pass->hdr_sampler_owned, NULL);
        }
        pb_rhi_buffer_destroy(pass->context, &pass->exposure_ubo);
        pb_rhi_buffer_destroy(pass->context, &pass->histogram_ssbo);
        if (pass->histogram_module) {
            vkDestroyShaderModule(device, pass->histogram_module, NULL);
        }
        if (pass->average_module) {
            vkDestroyShaderModule(device, pass->average_module, NULL);
        }
    }
    free(pass);
}

void pb_pbr_exposure_pass_record(
    pb_pbr_exposure_pass *pass,
    VkCommandBuffer cmd,
    VkImageView hdr_scene_view,
    VkSampler hdr_scene_sampler,
    VkExtent2D extent,
    float dt_seconds)
{
    if (!pass || !pass->histogram_pipeline || hdr_scene_view == VK_NULL_HANDLE) {
        return;
    }

    VkDevice device = pb_context_device(pass->context);

    /* Resolve the sampler — own one lazily if the caller didn't pass any. */
    if (hdr_scene_sampler == VK_NULL_HANDLE) {
        if (pass->hdr_sampler_owned == VK_NULL_HANDLE) {
            if (!create_default_sampler(device, &pass->hdr_sampler_owned)) {
                return;
            }
        }
        hdr_scene_sampler = pass->hdr_sampler_owned;
    }

    /* Update the histogram set's HDR binding only when it changes (the view
     * usually stable across frames). Avoids re-writing a descriptor a previous
     * frame's pending command buffer references. */
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
            .dstSet = pass->histogram_set,
            .dstBinding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .pImageInfo = &image_info,
        };
        vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
        pass->bound_view = hdr_scene_view;
        pass->bound_sampler = hdr_scene_sampler;
        pass->descriptor_written = true;
    }

    pass->params.dt = dt_seconds > 0.0f ? dt_seconds : 0.0f;

    /* 1. Clear the histogram SSBO. */
    vkCmdFillBuffer(cmd, pass->histogram_ssbo.handle, 0, PB_EXPOSURE_HISTOGRAM_BYTES, 0);

    /* Barrier: clear (TRANSFER_WRITE) must complete before the histogram read.
     * Also transition the HDR image layout implicitly via the descriptor's
     * declared layout — the caller has already moved it to SHADER_READ_ONLY. */
    VkBufferMemoryBarrier clear_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = pass->histogram_ssbo.handle,
        .offset = 0,
        .size = PB_EXPOSURE_HISTOGRAM_BYTES,
    };
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0, NULL,
        1, &clear_barrier,
        0, NULL);

    /* 2. Dispatch histogram. */
    const uint32_t groups_x = (extent.width + PB_EXPOSURE_WG_X - 1) / PB_EXPOSURE_WG_X;
    const uint32_t groups_y = (extent.height + PB_EXPOSURE_WG_Y - 1) / PB_EXPOSURE_WG_Y;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass->histogram_pipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        pass->histogram_pipeline_layout,
        0,
        1,
        &pass->histogram_set,
        0,
        NULL);
    vkCmdPushConstants(cmd, pass->histogram_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pb_exposure_push), &pass->params);
    vkCmdDispatch(cmd, groups_x, groups_y, 1);

    /* 3. Barrier: histogram writes must complete before the reduction reads. */
    VkBufferMemoryBarrier reduce_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = pass->histogram_ssbo.handle,
        .offset = 0,
        .size = PB_EXPOSURE_HISTOGRAM_BYTES,
    };
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0, NULL,
        1, &reduce_barrier,
        0, NULL);

    /* 4. Dispatch the reduction. */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass->average_pipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        pass->average_pipeline_layout,
        0,
        1,
        &pass->average_set,
        0,
        NULL);
    vkCmdPushConstants(cmd, pass->average_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pb_exposure_push), &pass->params);
    vkCmdDispatch(cmd, 1, 1, 1);

    /* 5. Barrier: the compute write to the exposure SSBO must complete before
     * the post pass's fragment shader reads it as a storage buffer. Without
     * this the validator flags a write-after-read hazard and the tonemap may
     * sample a stale exposure value from the previous frame. */
    VkBufferMemoryBarrier ubo_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = pass->exposure_ubo.handle,
        .offset = 0,
        .size = PB_EXPOSURE_UBO_BYTES,
    };
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0, NULL,
        1, &ubo_barrier,
        0, NULL);
}

float pb_pbr_exposure_pass_current(pb_pbr_exposure_pass *pass)
{
    if (!pass || !pass->exposure_ubo.mapped) {
        return 0.0f;
    }
    float value;
    memcpy(&value, pass->exposure_ubo.mapped, sizeof(value));
    return value;
}

VkBuffer pb_pbr_exposure_pass_ubo_handle(const pb_pbr_exposure_pass *pass)
{
    return pass ? pass->exposure_ubo.handle : VK_NULL_HANDLE;
}
