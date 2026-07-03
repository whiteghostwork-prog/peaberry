/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_RHI_RT_ACCEL_H
#define PEABERRY_RHI_RT_ACCEL_H

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_math.h"
#include "pbr/vertex.h"
#include "rhi/buffer.h"

#include <stdbool.h>
#include <stdint.h>
#include <volk.h>

typedef struct pb_rhi_blas {
    VkAccelerationStructureKHR handle;
    pb_rhi_buffer storage;
    pb_rhi_buffer vertices;
    pb_rhi_buffer indices;
    VkDeviceAddress vertex_address;
    VkDeviceAddress index_address;
    uint32_t index_count;
    uint32_t vertex_count;
    VkIndexType index_type;
} pb_rhi_blas;

typedef struct pb_rhi_tlas {
    VkAccelerationStructureKHR handle;
    pb_rhi_buffer storage;
    pb_rhi_buffer instances;
    uint32_t instance_count;
} pb_rhi_tlas;

typedef struct pb_rhi_blas_desc {
    const pb_pbr_vertex *vertices;
    uint32_t vertex_count;
    const void *indices;
    uint32_t index_count;
    VkIndexType index_type;
} pb_rhi_blas_desc;

typedef struct pb_rhi_tlas_instance {
    uint32_t blas_index;
    pb_mat4 transform;
    uint32_t custom_index;
    uint8_t mask;
} pb_rhi_tlas_instance;

bool pb_rhi_blas_create(
    pb_context *context,
    const pb_rhi_blas_desc *desc,
    pb_rhi_blas *blas);

void pb_rhi_blas_destroy(pb_context *context, pb_rhi_blas *blas);

bool pb_rhi_tlas_create(
    pb_context *context,
    const pb_rhi_blas *const *blases,
    const pb_rhi_tlas_instance *instances,
    uint32_t instance_count,
    pb_rhi_tlas *tlas);

bool pb_rhi_tlas_update(
    pb_context *context,
    const pb_rhi_blas *const *blases,
    const pb_rhi_tlas_instance *instances,
    uint32_t instance_count,
    pb_rhi_tlas *tlas);

void pb_rhi_tlas_destroy(pb_context *context, pb_rhi_tlas *tlas);

VkDeviceAddress pb_rhi_buffer_device_address(
    pb_context *context,
    const pb_rhi_buffer *buffer);

#endif
