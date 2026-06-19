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

#ifndef PEABERRY_RHI_BUFFER_H
#define PEABERRY_RHI_BUFFER_H

#include "peaberry/peaberry.h"
#include "rhi/alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <volk.h>

typedef struct pb_rhi_buffer {
    VkBuffer handle;
    VkDeviceMemory memory;
    VkDeviceSize size;
    bool host_visible;
    void *mapped;
} pb_rhi_buffer;

typedef struct pb_rhi_buffer_desc {
    VkDeviceSize size;
    VkBufferUsageFlags usage;
    pb_rhi_memory_usage memory_usage;
} pb_rhi_buffer_desc;

bool pb_rhi_buffer_create(
    pb_context *context,
    const pb_rhi_buffer_desc *desc,
    pb_rhi_buffer *buffer);
void pb_rhi_buffer_destroy(pb_context *context, pb_rhi_buffer *buffer);

VkBuffer pb_rhi_buffer_handle(const pb_rhi_buffer *buffer);

void *pb_rhi_buffer_mapped(const pb_rhi_buffer *buffer);

bool pb_rhi_buffer_write(
    pb_rhi_buffer *buffer,
    VkDeviceSize offset,
    const void *data,
    VkDeviceSize size);

bool pb_rhi_buffer_upload(
    pb_context *context,
    pb_rhi_buffer *buffer,
    const void *data,
    VkDeviceSize size);

#endif
