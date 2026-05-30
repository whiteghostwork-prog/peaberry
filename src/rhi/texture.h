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

#ifndef PEABERRY_RHI_TEXTURE_H
#define PEABERRY_RHI_TEXTURE_H

#include "peaberry/peaberry.h"

#include <stdbool.h>
#include <volk.h>

typedef struct pb_rhi_texture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSampler sampler;
    uint32_t width;
    uint32_t height;
} pb_rhi_texture;

bool pb_rhi_texture_create_from_file(
    pb_context *context,
    const char *path,
    pb_rhi_texture *texture);
void pb_rhi_texture_destroy(pb_context *context, pb_rhi_texture *texture);

#endif
