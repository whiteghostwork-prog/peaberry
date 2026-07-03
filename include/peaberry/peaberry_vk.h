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

#ifndef PEABERRY_VK_H
#define PEABERRY_VK_H

#include "peaberry/peaberry.h"

#include <volk.h>

bool pb_context_init_device(pb_context *ctx, VkSurfaceKHR surface);
bool pb_context_init_headless_device(pb_context *ctx);

VkInstance pb_context_instance(const pb_context *ctx);
VkPhysicalDevice pb_context_physical_device(const pb_context *ctx);
VkDevice pb_context_device(const pb_context *ctx);
VkQueue pb_context_graphics_queue(const pb_context *ctx);
VkQueue pb_context_present_queue(const pb_context *ctx);
uint32_t pb_context_graphics_queue_family(const pb_context *ctx);
uint32_t pb_context_present_queue_family(const pb_context *ctx);
bool pb_context_device_ready(const pb_context *ctx);
void pb_context_wait_device_idle(pb_context *ctx);

VkSampleCountFlagBits pb_context_choose_msaa_samples(
    const pb_context *ctx,
    VkSampleCountFlagBits requested_max);

bool pb_context_raytracing_supported(const pb_context *ctx);

#endif
