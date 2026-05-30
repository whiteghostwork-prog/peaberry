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
#include "vk/context.h"

#include <stdlib.h>

struct pb_context {
    pb_vk_context vk;
};

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
