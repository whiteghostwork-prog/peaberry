/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pbr/rt_scene.h"

#include "core/log.h"
#include "peaberry/peaberry_vk.h"
#include "rhi/rt_accel.h"

#include <stdlib.h>
#include <string.h>

struct pb_rt_scene {
    pb_context *context;
    pb_rhi_blas *blases;
    uint32_t blas_count;
    pb_rhi_tlas tlas;
    uint32_t *draw_to_blas;
    pb_rhi_tlas_instance *instances;
    uint32_t *draw_to_instance;
    uint32_t instance_count;
};

static int find_blas_for_mesh(const pb_rt_scene *scene, VkBuffer vertex_buffer)
{
    for (uint32_t i = 0; i < scene->blas_count; ++i) {
        if (scene->blases[i].vertices.handle == vertex_buffer) {
            return (int)i;
        }
    }

    return -1;
}

static bool create_blas_from_draw(
    pb_rt_scene *scene,
    const pb_gltf_draw *draw,
    uint32_t *out_blas_index)
{
    const pb_rhi_mesh *mesh = &draw->mesh;
    const void *vertices = pb_rhi_buffer_mapped(&mesh->vertices);
    const void *indices = pb_rhi_buffer_mapped(&mesh->indices);

    if (!vertices || !indices || mesh->index_count < 3) {
        return false;
    }

    const uint32_t vertex_count = (uint32_t)(mesh->vertices.size / sizeof(pb_pbr_vertex));
    if (vertex_count == 0) {
        return false;
    }

    pb_rhi_blas *blases =
        realloc(scene->blases, (size_t)(scene->blas_count + 1) * sizeof(*scene->blases));
    if (!blases) {
        return false;
    }

    scene->blases = blases;
    pb_rhi_blas *blas = &scene->blases[scene->blas_count];
    memset(blas, 0, sizeof(*blas));

    if (!pb_rhi_blas_create(
            scene->context,
            &(pb_rhi_blas_desc){
                .vertices = vertices,
                .vertex_count = vertex_count,
                .indices = indices,
                .index_count = mesh->index_count,
                .index_type = mesh->index_type,
            },
            blas)) {
        return false;
    }

    *out_blas_index = scene->blas_count;
    scene->blas_count++;
    return true;
}

pb_rt_scene *pb_rt_scene_create(pb_context *context, const pb_gltf_scene *scene)
{
    if (!context || !scene || scene->draw_count == 0 || !pb_context_raytracing_supported(context)) {
        return NULL;
    }

    pb_rt_scene *rt = calloc(1, sizeof(*rt));
    if (!rt) {
        return NULL;
    }

    rt->context = context;
    rt->draw_to_blas = calloc(scene->draw_count, sizeof(*rt->draw_to_blas));
    rt->draw_to_instance = malloc(scene->draw_count * sizeof(*rt->draw_to_instance));
    if (!rt->draw_to_blas || !rt->draw_to_instance) {
        pb_rt_scene_destroy(rt);
        return NULL;
    }

    for (uint32_t i = 0; i < scene->draw_count; ++i) {
        rt->draw_to_instance[i] = UINT32_MAX;
    }

    for (uint32_t i = 0; i < scene->draw_count; ++i) {
        const pb_gltf_draw *draw = &scene->draws[i];
        const pb_gltf_material *material = &scene->materials[draw->material_index];

        if (material->alpha_mode == PB_GLTF_ALPHA_BLEND) {
            continue;
        }

        int blas_index = find_blas_for_mesh(rt, draw->mesh.vertices.handle);
        if (blas_index < 0) {
            uint32_t new_index = 0;
            if (!create_blas_from_draw(rt, draw, &new_index)) {
                pb_log_error("Failed to build BLAS for draw %u", i);
                pb_rt_scene_destroy(rt);
                return NULL;
            }
            blas_index = (int)new_index;
        }

        rt->draw_to_blas[i] = (uint32_t)blas_index;
    }

    if (!pb_rt_scene_update(rt, scene)) {
        pb_rt_scene_destroy(rt);
        return NULL;
    }

    pb_log_info("RT scene ready: %u BLAS, %u TLAS instances", rt->blas_count, rt->instance_count);
    return rt;
}

bool pb_rt_scene_update(pb_rt_scene *scene, const pb_gltf_scene *gltf_scene)
{
    if (!scene || !gltf_scene) {
        return false;
    }

    uint32_t opaque_count = 0;
    for (uint32_t i = 0; i < gltf_scene->draw_count; ++i) {
        const pb_gltf_material *material = &gltf_scene->materials[gltf_scene->draws[i].material_index];
        if (material->alpha_mode != PB_GLTF_ALPHA_BLEND) {
            opaque_count++;
        }
    }

    if (opaque_count == 0) {
        return false;
    }

    pb_rhi_tlas_instance *instances = realloc(scene->instances, opaque_count * sizeof(*instances));
    if (!instances) {
        return false;
    }

    scene->instances = instances;

    uint32_t instance_index = 0;
    for (uint32_t i = 0; i < gltf_scene->draw_count; ++i) {
        const pb_gltf_draw *draw = &gltf_scene->draws[i];
        const pb_gltf_material *material = &gltf_scene->materials[draw->material_index];

        if (material->alpha_mode == PB_GLTF_ALPHA_BLEND) {
            scene->draw_to_instance[i] = UINT32_MAX;
            continue;
        }

        scene->instances[instance_index] = (pb_rhi_tlas_instance){
            .blas_index = scene->draw_to_blas[i],
            .custom_index = draw->material_index,
            .mask = 0xFF,
        };
        memcpy(scene->instances[instance_index].transform, draw->world, sizeof(draw->world));
        scene->draw_to_instance[i] = instance_index;
        instance_index++;
    }

    const pb_rhi_blas **blas_ptrs = calloc(scene->blas_count, sizeof(*blas_ptrs));
    if (!blas_ptrs) {
        return false;
    }

    for (uint32_t i = 0; i < scene->blas_count; ++i) {
        blas_ptrs[i] = &scene->blases[i];
    }

    bool ok = false;
    if (scene->tlas.handle == VK_NULL_HANDLE) {
        ok = pb_rhi_tlas_create(scene->context, blas_ptrs, scene->instances, opaque_count, &scene->tlas);
    } else if (opaque_count == scene->instance_count) {
        ok = pb_rhi_tlas_update(scene->context, blas_ptrs, scene->instances, opaque_count, &scene->tlas);
    } else {
        pb_rhi_tlas_destroy(scene->context, &scene->tlas);
        ok = pb_rhi_tlas_create(scene->context, blas_ptrs, scene->instances, opaque_count, &scene->tlas);
    }

    free(blas_ptrs);

    if (ok) {
        scene->instance_count = opaque_count;
    }

    return ok;
}

VkAccelerationStructureKHR pb_rt_scene_tlas(const pb_rt_scene *scene)
{
    return scene ? scene->tlas.handle : VK_NULL_HANDLE;
}

void pb_rt_scene_destroy(pb_rt_scene *scene)
{
    if (!scene) {
        return;
    }

    if (scene->context && pb_context_device_ready(scene->context)) {
        pb_rhi_tlas_destroy(scene->context, &scene->tlas);
        for (uint32_t i = 0; i < scene->blas_count; ++i) {
            pb_rhi_blas_destroy(scene->context, &scene->blases[i]);
        }
    }

    free(scene->blases);
    free(scene->draw_to_blas);
    free(scene->draw_to_instance);
    free(scene->instances);
    free(scene);
}
