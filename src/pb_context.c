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

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_vk.h"

#include "core/log.h"
#include "pb_context_internal.h"
#include "vk/context.h"

#include <stddef.h>
#include <stdlib.h>

pb_context *pb_context_create(const pb_context_desc *desc)
{
    if (!desc || !desc->app_name) {
        pb_log_error("Invalid context description");
        return NULL;
    }

    pb_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    if (!pb_vk_context_init_instance(
            &ctx->vk,
            desc->app_name,
            desc->enable_validation,
            desc->enable_surface)) {
        free(ctx);
        return NULL;
    }

    return ctx;
}

void pb_context_destroy(pb_context *ctx)
{
    if (!ctx) {
        return;
    }

    pb_vk_context_shutdown(&ctx->vk);
    free(ctx);
}

bool pb_context_init_device(pb_context *ctx, VkSurfaceKHR surface)
{
    if (!ctx) {
        return false;
    }

    if (!ctx->vk.surface_enabled) {
        pb_log_error("Context was created without surface support");
        return false;
    }

    if (surface == VK_NULL_HANDLE) {
        pb_log_error("Invalid surface");
        return false;
    }

    return pb_vk_context_init_device(&ctx->vk, surface);
}

bool pb_context_init_headless_device(pb_context *ctx)
{
    if (!ctx) {
        return false;
    }

    if (ctx->vk.surface_enabled) {
        pb_log_error("Headless device init requires enable_surface = false");
        return false;
    }

    return pb_vk_context_init_headless_device(&ctx->vk);
}

VkSampleCountFlagBits pb_context_choose_msaa_samples(
    const pb_context *ctx,
    VkSampleCountFlagBits requested_max)
{
    if (!ctx || ctx->vk.physical_device == VK_NULL_HANDLE) {
        return VK_SAMPLE_COUNT_1_BIT;
    }

    if (requested_max == 0) {
        requested_max = VK_SAMPLE_COUNT_1_BIT;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(ctx->vk.physical_device, &props);

    const VkSampleCountFlags supported = props.limits.framebufferColorSampleCounts &
        props.limits.framebufferDepthSampleCounts;

    const VkSampleCountFlagBits candidates[] = {
        VK_SAMPLE_COUNT_4_BIT,
        VK_SAMPLE_COUNT_2_BIT,
        VK_SAMPLE_COUNT_1_BIT,
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (candidates[i] <= requested_max && (supported & candidates[i])) {
            return candidates[i];
        }
    }

    return VK_SAMPLE_COUNT_1_BIT;
}

VkInstance pb_context_instance(const pb_context *ctx)
{
    return ctx ? ctx->vk.instance : VK_NULL_HANDLE;
}

VkPhysicalDevice pb_context_physical_device(const pb_context *ctx)
{
    return ctx ? ctx->vk.physical_device : VK_NULL_HANDLE;
}

VkDevice pb_context_device(const pb_context *ctx)
{
    return ctx ? ctx->vk.device : VK_NULL_HANDLE;
}

VkQueue pb_context_graphics_queue(const pb_context *ctx)
{
    return ctx ? ctx->vk.graphics_queue : VK_NULL_HANDLE;
}

VkQueue pb_context_present_queue(const pb_context *ctx)
{
    return ctx ? ctx->vk.present_queue : VK_NULL_HANDLE;
}

uint32_t pb_context_graphics_queue_family(const pb_context *ctx)
{
    return ctx ? ctx->vk.graphics_queue_family : 0;
}

uint32_t pb_context_present_queue_family(const pb_context *ctx)
{
    return ctx ? ctx->vk.present_queue_family : 0;
}

bool pb_context_device_ready(const pb_context *ctx)
{
    return ctx && ctx->vk.device != VK_NULL_HANDLE;
}

void pb_context_wait_device_idle(pb_context *ctx)
{
    if (!pb_context_device_ready(ctx)) {
        return;
    }

    vkDeviceWaitIdle(ctx->vk.device);
}

bool pb_context_raytracing_supported(const pb_context *ctx)
{
#ifdef PEABERRY_ENABLE_RAYTRACING
    return ctx && ctx->vk.raytracing_supported;
#else
    (void)ctx;
    return false;
#endif
}
