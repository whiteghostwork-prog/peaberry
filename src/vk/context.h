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

#ifndef PEABERRY_VK_CONTEXT_H
#define PEABERRY_VK_CONTEXT_H

#include <stdbool.h>
#include <volk.h>

typedef struct pb_vk_context {
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue graphics_queue;
    VkQueue present_queue;
    uint32_t graphics_queue_family;
    uint32_t present_queue_family;
    bool validation_enabled;
    bool surface_enabled;
} pb_vk_context;

bool pb_vk_context_init_instance(
    pb_vk_context *ctx,
    const char *app_name,
    bool enable_validation,
    bool enable_surface);
bool pb_vk_context_init_device(pb_vk_context *ctx, VkSurfaceKHR surface);
bool pb_vk_context_init_headless_device(pb_vk_context *ctx);
void pb_vk_context_shutdown(pb_vk_context *ctx);

#endif
