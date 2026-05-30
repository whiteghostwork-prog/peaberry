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

#ifndef PEABERRY_RHI_ALLOC_H
#define PEABERRY_RHI_ALLOC_H

#include "vk/context.h"

#include <stdbool.h>
#include <volk.h>

typedef enum pb_rhi_memory_usage {
    PB_RHI_MEMORY_CPU_TO_GPU,
    PB_RHI_MEMORY_GPU_ONLY,
} pb_rhi_memory_usage;

bool pb_rhi_alloc_init(pb_vk_context *ctx);
void pb_rhi_alloc_shutdown(pb_vk_context *ctx);

uint32_t pb_rhi_find_memory_type(
    const pb_vk_context *ctx,
    uint32_t type_filter,
    VkMemoryPropertyFlags properties);

VkMemoryPropertyFlags pb_rhi_memory_properties(pb_rhi_memory_usage usage);

#endif
