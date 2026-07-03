/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rhi/rt_accel.h"

#include "core/log.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/alloc.h"
#include "rhi/cmd_submit.h"

#include <stdlib.h>
#include <string.h>

static void mat4_to_vk_transform(const pb_mat4 m, VkTransformMatrixKHR *out)
{
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            out->matrix[row][col] = m[row][col];
        }
    }
}

VkDeviceAddress pb_rhi_buffer_device_address(
    pb_context *context,
    const pb_rhi_buffer *buffer)
{
    if (!context || !buffer || buffer->handle == VK_NULL_HANDLE) {
        return 0;
    }

    VkBufferDeviceAddressInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer->handle,
    };

    return vkGetBufferDeviceAddress(pb_context_device(context), &info);
}

static bool create_device_local_buffer(
    pb_context *context,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    pb_rhi_buffer *buffer)
{
    const pb_rhi_buffer_desc desc = {
        .size = size,
        .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };

    return pb_rhi_buffer_create(context, &desc, buffer);
}

static bool get_as_properties(
    pb_context *context,
    VkPhysicalDeviceAccelerationStructurePropertiesKHR *props)
{
    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = props,
    };

    props->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    vkGetPhysicalDeviceProperties2(pb_context_physical_device(context), &props2);
    return true;
}

typedef struct pb_as_build_payload {
    VkAccelerationStructureBuildGeometryInfoKHR build_info;
    VkAccelerationStructureGeometryKHR geometry;
    VkAccelerationStructureGeometryTrianglesDataKHR triangles;
    VkAccelerationStructureBuildRangeInfoKHR range;
} pb_as_build_payload;

typedef struct pb_tlas_build_payload {
    VkAccelerationStructureBuildGeometryInfoKHR build_info;
    VkAccelerationStructureGeometryKHR geometry;
    VkAccelerationStructureGeometryInstancesDataKHR instances_data;
    VkAccelerationStructureBuildRangeInfoKHR range;
} pb_tlas_build_payload;

static void record_blas_build(VkCommandBuffer cmd, void *user_data)
{
    pb_as_build_payload *payload = user_data;
    const VkAccelerationStructureBuildRangeInfoKHR *ranges[] = { &payload->range };
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &payload->build_info, ranges);
}

static bool build_blas_internal(
    pb_context *context,
    pb_rhi_blas *blas,
    VkDeviceSize scratch_size,
    pb_rhi_buffer *scratch)
{
    VkAccelerationStructureBuildGeometryInfoKHR build_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .dstAccelerationStructure = blas->handle,
        .geometryCount = 1,
    };

    pb_as_build_payload payload = {0};
    payload.triangles = (VkAccelerationStructureGeometryTrianglesDataKHR){
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
        .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
        .vertexData.deviceAddress = blas->vertex_address,
        .vertexStride = sizeof(pb_pbr_vertex),
        .maxVertex = blas->vertex_count > 0 ? blas->vertex_count - 1 : 0,
        .indexType = blas->index_type,
        .indexData.deviceAddress = blas->index_address,
    };
    payload.geometry = (VkAccelerationStructureGeometryKHR){
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry.triangles = payload.triangles,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
    };
    build_info.pGeometries = &payload.geometry;

    uint32_t primitive_count = blas->index_count / 3;
    VkAccelerationStructureBuildSizesInfoKHR size_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
    };

    vkGetAccelerationStructureBuildSizesKHR(
        pb_context_device(context),
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &build_info,
        &primitive_count,
        &size_info);

    if (scratch_size < size_info.buildScratchSize) {
        pb_log_error("BLAS scratch buffer too small");
        return false;
    }

    payload.build_info = build_info;
    payload.build_info.scratchData.deviceAddress = pb_rhi_buffer_device_address(context, scratch);
    payload.range = (VkAccelerationStructureBuildRangeInfoKHR){
        .primitiveCount = primitive_count,
    };

    return pb_rhi_submit_one_shot(context, record_blas_build, &payload);
}

bool pb_rhi_blas_create(
    pb_context *context,
    const pb_rhi_blas_desc *desc,
    pb_rhi_blas *blas)
{
    if (!context || !desc || !blas || !desc->vertices || !desc->indices || desc->vertex_count == 0 ||
        desc->index_count == 0) {
        return false;
    }

    memset(blas, 0, sizeof(*blas));
    blas->index_count = desc->index_count;
    blas->index_type = desc->index_type;
    blas->vertex_count = desc->vertex_count;

    const size_t vertex_bytes = (size_t)desc->vertex_count * sizeof(pb_pbr_vertex);
    const size_t index_bytes =
        desc->index_type == VK_INDEX_TYPE_UINT16 ? (size_t)desc->index_count * sizeof(uint16_t)
                                                 : (size_t)desc->index_count * sizeof(uint32_t);

    if (!create_device_local_buffer(
            context,
            vertex_bytes,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            &blas->vertices) ||
        !create_device_local_buffer(
            context,
            index_bytes,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            &blas->indices)) {
        pb_rhi_blas_destroy(context, blas);
        return false;
    }

    if (!pb_rhi_buffer_upload(context, &blas->vertices, desc->vertices, vertex_bytes) ||
        !pb_rhi_buffer_upload(context, &blas->indices, desc->indices, index_bytes)) {
        pb_rhi_blas_destroy(context, blas);
        return false;
    }

    blas->vertex_address = pb_rhi_buffer_device_address(context, &blas->vertices);
    blas->index_address = pb_rhi_buffer_device_address(context, &blas->indices);

    VkAccelerationStructureGeometryTrianglesDataKHR triangles = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
        .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
        .vertexData.deviceAddress = blas->vertex_address,
        .vertexStride = sizeof(pb_pbr_vertex),
        .maxVertex = desc->vertex_count - 1,
        .indexType = blas->index_type,
        .indexData.deviceAddress = blas->index_address,
    };

    VkAccelerationStructureGeometryKHR geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry.triangles = triangles,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
    };

    VkAccelerationStructureBuildGeometryInfoKHR build_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };

    uint32_t primitive_count = desc->index_count / 3;
    VkAccelerationStructureBuildSizesInfoKHR size_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
    };

    vkGetAccelerationStructureBuildSizesKHR(
        pb_context_device(context),
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &build_info,
        &primitive_count,
        &size_info);

    VkPhysicalDeviceAccelerationStructurePropertiesKHR as_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
    };
    get_as_properties(context, &as_props);

    const VkDeviceSize storage_size =
        pb_rhi_align_up(size_info.accelerationStructureSize, as_props.minAccelerationStructureScratchOffsetAlignment);

    if (!create_device_local_buffer(
            context,
            storage_size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            &blas->storage)) {
        pb_rhi_blas_destroy(context, blas);
        return false;
    }

    VkAccelerationStructureCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = blas->storage.handle,
        .size = size_info.accelerationStructureSize,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
    };

    if (vkCreateAccelerationStructureKHR(pb_context_device(context), &create_info, NULL, &blas->handle) !=
        VK_SUCCESS) {
        pb_rhi_blas_destroy(context, blas);
        return false;
    }

    pb_rhi_buffer scratch = {0};
    if (!create_device_local_buffer(
            context,
            size_info.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            &scratch) ||
        !build_blas_internal(context, blas, size_info.buildScratchSize, &scratch)) {
        pb_rhi_buffer_destroy(context, &scratch);
        pb_rhi_blas_destroy(context, blas);
        return false;
    }

    pb_rhi_buffer_destroy(context, &scratch);
    return true;
}

void pb_rhi_blas_destroy(pb_context *context, pb_rhi_blas *blas)
{
    if (!blas) {
        return;
    }

    if (context && pb_context_device_ready(context) && blas->handle) {
        vkDestroyAccelerationStructureKHR(pb_context_device(context), blas->handle, NULL);
    }

    if (context) {
        pb_rhi_buffer_destroy(context, &blas->storage);
        pb_rhi_buffer_destroy(context, &blas->vertices);
        pb_rhi_buffer_destroy(context, &blas->indices);
    }

    memset(blas, 0, sizeof(*blas));
}

static void record_tlas_build(VkCommandBuffer cmd, void *user_data)
{
    pb_tlas_build_payload *payload = user_data;
    const VkAccelerationStructureBuildRangeInfoKHR *ranges[] = { &payload->range };
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &payload->build_info, ranges);
}

static bool build_tlas_internal(
    pb_context *context,
    pb_rhi_tlas *tlas,
    bool update,
    VkDeviceSize scratch_size,
    pb_rhi_buffer *scratch)
{
    pb_tlas_build_payload payload = {0};

    payload.instances_data = (VkAccelerationStructureGeometryInstancesDataKHR){
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
        .arrayOfPointers = VK_FALSE,
        .data.deviceAddress = pb_rhi_buffer_device_address(context, &tlas->instances),
    };
    payload.geometry = (VkAccelerationStructureGeometryKHR){
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = payload.instances_data,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
    };

    payload.build_info = (VkAccelerationStructureBuildGeometryInfoKHR){
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
            (update ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0),
        .mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                       : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .srcAccelerationStructure = update ? tlas->handle : VK_NULL_HANDLE,
        .dstAccelerationStructure = tlas->handle,
        .geometryCount = 1,
        .pGeometries = &payload.geometry,
    };

    payload.range = (VkAccelerationStructureBuildRangeInfoKHR){
        .primitiveCount = tlas->instance_count,
    };

    if (scratch_size > 0) {
        payload.build_info.scratchData.deviceAddress = pb_rhi_buffer_device_address(context, scratch);
    }

    return pb_rhi_submit_one_shot(context, record_tlas_build, &payload);
}

static bool fill_instance_buffer(
    pb_context *context,
    pb_rhi_buffer *instances,
    const pb_rhi_blas *const *blases,
    const pb_rhi_tlas_instance *instances_desc,
    uint32_t instance_count)
{
    VkAccelerationStructureInstanceKHR *data = pb_rhi_buffer_mapped(instances);
    if (!data) {
        return false;
    }

    for (uint32_t i = 0; i < instance_count; ++i) {
        const pb_rhi_tlas_instance *src = &instances_desc[i];
        VkAccelerationStructureInstanceKHR *dst = &data[i];

        mat4_to_vk_transform(src->transform, &dst->transform);
        dst->instanceCustomIndex = src->custom_index;
        dst->mask = src->mask;
        dst->instanceShaderBindingTableRecordOffset = 0;
        dst->flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        dst->accelerationStructureReference =
            vkGetAccelerationStructureDeviceAddressKHR(
                pb_context_device(context),
                &(VkAccelerationStructureDeviceAddressInfoKHR){
                    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                    .accelerationStructure = blases[src->blas_index]->handle,
                });
    }

    return true;
}

static bool create_tlas_storage(
    pb_context *context,
    uint32_t instance_count,
    pb_rhi_tlas *tlas,
    VkAccelerationStructureBuildSizesInfoKHR *size_info)
{
    VkAccelerationStructureGeometryInstancesDataKHR instances_data = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
        .arrayOfPointers = VK_FALSE,
        .data.deviceAddress = pb_rhi_buffer_device_address(context, &tlas->instances),
    };

    VkAccelerationStructureGeometryKHR geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = instances_data,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
    };

    VkAccelerationStructureBuildGeometryInfoKHR build_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
            VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };

    vkGetAccelerationStructureBuildSizesKHR(
        pb_context_device(context),
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &build_info,
        &instance_count,
        size_info);

    VkPhysicalDeviceAccelerationStructurePropertiesKHR as_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
    };
    get_as_properties(context, &as_props);

    const VkDeviceSize storage_size = pb_rhi_align_up(
        size_info->accelerationStructureSize,
        as_props.minAccelerationStructureScratchOffsetAlignment);

    return create_device_local_buffer(
        context,
        storage_size,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        &tlas->storage);
}

bool pb_rhi_tlas_create(
    pb_context *context,
    const pb_rhi_blas *const *blases,
    const pb_rhi_tlas_instance *instances,
    uint32_t instance_count,
    pb_rhi_tlas *tlas)
{
    if (!context || !blases || !instances || instance_count == 0 || !tlas) {
        return false;
    }

    memset(tlas, 0, sizeof(*tlas));
    tlas->instance_count = instance_count;

    const VkDeviceSize instance_bytes = (VkDeviceSize)instance_count * sizeof(VkAccelerationStructureInstanceKHR);
    const pb_rhi_buffer_desc instance_desc = {
        .size = instance_bytes,
        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };

    if (!pb_rhi_buffer_create(context, &instance_desc, &tlas->instances) ||
        !fill_instance_buffer(context, &tlas->instances, blases, instances, instance_count)) {
        pb_rhi_tlas_destroy(context, tlas);
        return false;
    }

    VkAccelerationStructureBuildSizesInfoKHR size_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
    };

    if (!create_tlas_storage(context, instance_count, tlas, &size_info)) {
        pb_rhi_tlas_destroy(context, tlas);
        return false;
    }

    VkAccelerationStructureCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = tlas->storage.handle,
        .size = size_info.accelerationStructureSize,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
    };

    if (vkCreateAccelerationStructureKHR(pb_context_device(context), &create_info, NULL, &tlas->handle) !=
        VK_SUCCESS) {
        pb_rhi_tlas_destroy(context, tlas);
        return false;
    }

    pb_rhi_buffer scratch = {0};
    if (!create_device_local_buffer(
            context,
            size_info.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            &scratch) ||
        !build_tlas_internal(context, tlas, false, size_info.buildScratchSize, &scratch)) {
        pb_rhi_buffer_destroy(context, &scratch);
        pb_rhi_tlas_destroy(context, tlas);
        return false;
    }

    pb_rhi_buffer_destroy(context, &scratch);
    return true;
}

bool pb_rhi_tlas_update(
    pb_context *context,
    const pb_rhi_blas *const *blases,
    const pb_rhi_tlas_instance *instances,
    uint32_t instance_count,
    pb_rhi_tlas *tlas)
{
    if (!context || !blases || !instances || !tlas || tlas->handle == VK_NULL_HANDLE ||
        instance_count != tlas->instance_count) {
        return false;
    }

    if (!fill_instance_buffer(context, &tlas->instances, blases, instances, instance_count)) {
        return false;
    }

    VkAccelerationStructureGeometryInstancesDataKHR instances_data = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
        .arrayOfPointers = VK_FALSE,
        .data.deviceAddress = pb_rhi_buffer_device_address(context, &tlas->instances),
    };

    VkAccelerationStructureGeometryKHR geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = instances_data,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
    };

    VkAccelerationStructureBuildGeometryInfoKHR build_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
            VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };

    VkAccelerationStructureBuildSizesInfoKHR size_info = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
    };

    vkGetAccelerationStructureBuildSizesKHR(
        pb_context_device(context),
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &build_info,
        &instance_count,
        &size_info);

    pb_rhi_buffer scratch = {0};
    if (!create_device_local_buffer(
            context,
            size_info.updateScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            &scratch) ||
        !build_tlas_internal(context, tlas, true, size_info.updateScratchSize, &scratch)) {
        pb_rhi_buffer_destroy(context, &scratch);
        return false;
    }

    pb_rhi_buffer_destroy(context, &scratch);
    return true;
}

void pb_rhi_tlas_destroy(pb_context *context, pb_rhi_tlas *tlas)
{
    if (!tlas) {
        return;
    }

    if (context && pb_context_device_ready(context) && tlas->handle) {
        vkDestroyAccelerationStructureKHR(pb_context_device(context), tlas->handle, NULL);
    }

    if (context) {
        pb_rhi_buffer_destroy(context, &tlas->storage);
        pb_rhi_buffer_destroy(context, &tlas->instances);
    }

    memset(tlas, 0, sizeof(*tlas));
}
