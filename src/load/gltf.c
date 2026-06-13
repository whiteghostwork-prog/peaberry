/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "peaberry/peaberry_gltf.h"

#include "core/log.h"
#include "load/tangent.h"
#include "pbr/gltf_scene_internal.h"
#include "pbr/vertex.h"
#include "rhi/mesh.h"
#include "rhi/texture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PB_PATH_MAX = 4096 };

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

static void path_dirname(const char *path, char *out, size_t out_size)
{
    if (!path || out_size == 0) {
        return;
    }

    strncpy(out, path, out_size - 1);
    out[out_size - 1] = '\0';

    char *slash = strrchr(out, '/');
    char *backslash = strrchr(out, '\\');
    char *sep = slash > backslash ? slash : backslash;
    if (sep) {
        *sep = '\0';
    } else {
        out[0] = '.';
        out[1] = '\0';
    }
}

static bool path_join(char *out, size_t out_size, const char *dir, const char *rel)
{
    if (!out || out_size == 0 || !dir || !rel) {
        return false;
    }

    if (rel[0] == '/' || (rel[0] && rel[1] == ':')) {
        return snprintf(out, out_size, "%s", rel) < (int)out_size;
    }

    const size_t dir_len = strlen(dir);
    const bool needs_sep = dir_len > 0 && dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\';
    return snprintf(out, out_size, needs_sep ? "%s/%s" : "%s%s", dir, rel) < (int)out_size;
}

static bool load_image_texture(
    pb_context *context,
    const cgltf_image *image,
    const char *gltf_dir,
    bool srgb,
    pb_rhi_texture *out)
{
    if (!image) {
        return false;
    }

    if (image->buffer_view) {
        const cgltf_buffer_view *view = image->buffer_view;
        const cgltf_buffer *buffer = view->buffer;
        const uint8_t *data = (const uint8_t *)buffer->data + view->offset;
        const size_t size = view->size > 0 ? view->size : buffer->size - view->offset;
        return pb_rhi_texture_create_from_memory(context, data, size, srgb, out);
    }

    if (!image->uri || image->uri[0] == '\0') {
        return false;
    }

    if (strncmp(image->uri, "data:", 5) == 0) {
        const char *comma = strchr(image->uri, ',');
        if (!comma) {
            return false;
        }
        /* stb does not decode base64; skip embedded data URIs for now. */
        (void)comma;
        return false;
    }

    char path[PB_PATH_MAX];
    if (!path_join(path, sizeof(path), gltf_dir, image->uri)) {
        return false;
    }

    return pb_rhi_texture_create_from_file(context, path, srgb, out);
}

static void set_uv_transform_identity(float a[4], float b[4])
{
    a[0] = 0.0f;
    a[1] = 0.0f;
    a[2] = 0.0f;
    a[3] = 0.0f;
    b[0] = 1.0f;
    b[1] = 1.0f;
    b[2] = 0.0f;
    b[3] = 0.0f;
}

static void load_uv_transform(const cgltf_texture_view *view, float a[4], float b[4])
{
    set_uv_transform_identity(a, b);

    if (!view || !view->has_transform) {
        return;
    }

    const cgltf_texture_transform *transform = &view->transform;
    a[0] = (float)transform->offset[0];
    a[1] = (float)transform->offset[1];
    a[2] = (float)transform->rotation;
    a[3] = 1.0f;
    b[0] = (float)transform->scale[0];
    b[1] = (float)transform->scale[1];
}

static void set_material_uv_transform(pb_gltf_material *dst, uint32_t slot, const cgltf_texture_view *view)
{
    if (slot >= 5) {
        return;
    }

    load_uv_transform(view, dst->material_data.uv_transform_a[slot], dst->material_data.uv_transform_b[slot]);
}

static bool load_material_textures(
    pb_context *context,
    const cgltf_material *src,
    const char *gltf_dir,
    pb_gltf_material *dst)
{
    const uint8_t white[4] = { 255, 255, 255, 255 };
    const uint8_t mr_default[4] = { 0, 128, 0, 255 };
    const uint8_t normal_default[4] = { 128, 128, 255, 255 };
    const uint8_t black[4] = { 0, 0, 0, 255 };

    if (!pb_rhi_texture_create_solid_rgba8(context, white, true, &dst->albedo) ||
        !pb_rhi_texture_create_solid_rgba8(context, mr_default, false, &dst->metallic_roughness) ||
        !pb_rhi_texture_create_solid_rgba8(context, normal_default, false, &dst->normal) ||
        !pb_rhi_texture_create_solid_rgba8(context, white, false, &dst->occlusion) ||
        !pb_rhi_texture_create_solid_rgba8(context, black, false, &dst->emissive)) {
        return false;
    }

    dst->material_data.occlusion_strength = 1.0f;
    dst->material_data.emissive_factor[0] = 0.0f;
    dst->material_data.emissive_factor[1] = 0.0f;
    dst->material_data.emissive_factor[2] = 0.0f;
    dst->material_data.alpha_cutoff = 0.5f;
    dst->material_data.base_color_alpha = 1.0f;
    dst->material_data.alpha_mode = (float)PB_GLTF_ALPHA_OPAQUE;
    dst->alpha_mode = PB_GLTF_ALPHA_OPAQUE;
    dst->double_sided = false;

    for (uint32_t slot = 0; slot < 5; ++slot) {
        set_uv_transform_identity(
            dst->material_data.uv_transform_a[slot],
            dst->material_data.uv_transform_b[slot]);
    }

    if (src) {
        dst->double_sided = src->double_sided ? true : false;
        dst->material_data.double_sided = dst->double_sided ? 1.0f : 0.0f;
        switch (src->alpha_mode) {
        case cgltf_alpha_mode_mask:
            dst->alpha_mode = PB_GLTF_ALPHA_MASK;
            dst->material_data.alpha_mode = (float)PB_GLTF_ALPHA_MASK;
            break;
        case cgltf_alpha_mode_blend:
            dst->alpha_mode = PB_GLTF_ALPHA_BLEND;
            dst->material_data.alpha_mode = (float)PB_GLTF_ALPHA_BLEND;
            break;
        default:
            break;
        }
        dst->material_data.alpha_cutoff = src->alpha_cutoff;
    }

    if (src && src->has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness *pbr = &src->pbr_metallic_roughness;
        dst->material_data.albedo_factor[0] = pbr->base_color_factor[0];
        dst->material_data.albedo_factor[1] = pbr->base_color_factor[1];
        dst->material_data.albedo_factor[2] = pbr->base_color_factor[2];
        dst->material_data.base_color_alpha = pbr->base_color_factor[3];
        dst->material_data.metallic_factor = pbr->metallic_factor;
        dst->material_data.roughness_factor = pbr->roughness_factor;

        if (pbr->base_color_texture.texture) {
            set_material_uv_transform(dst, PB_GLTF_TEXTURE_ALBEDO, &pbr->base_color_texture);
            pb_rhi_texture_destroy(context, &dst->albedo);
            if (!load_image_texture(
                    context,
                    pbr->base_color_texture.texture->image,
                    gltf_dir,
                    true,
                    &dst->albedo) &&
                !pb_rhi_texture_create_solid_rgba8(context, white, true, &dst->albedo)) {
                return false;
            }
        }

        if (pbr->metallic_roughness_texture.texture) {
            set_material_uv_transform(dst, PB_GLTF_TEXTURE_METALLIC_ROUGHNESS, &pbr->metallic_roughness_texture);
            pb_rhi_texture_destroy(context, &dst->metallic_roughness);
            if (!load_image_texture(
                    context,
                    pbr->metallic_roughness_texture.texture->image,
                    gltf_dir,
                    false,
                    &dst->metallic_roughness) &&
                !pb_rhi_texture_create_solid_rgba8(context, mr_default, false, &dst->metallic_roughness)) {
                return false;
            }
        }
    }

    if (src && src->normal_texture.texture) {
        set_material_uv_transform(dst, PB_GLTF_TEXTURE_NORMAL, &src->normal_texture);
        pb_rhi_texture_destroy(context, &dst->normal);
        if (!load_image_texture(
                context,
                src->normal_texture.texture->image,
                gltf_dir,
                false,
                &dst->normal) &&
            !pb_rhi_texture_create_solid_rgba8(context, normal_default, false, &dst->normal)) {
            return false;
        }
    }

    if (src && src->occlusion_texture.texture) {
        set_material_uv_transform(dst, PB_GLTF_TEXTURE_OCCLUSION, &src->occlusion_texture);
        pb_rhi_texture_destroy(context, &dst->occlusion);
        dst->material_data.occlusion_strength = src->occlusion_texture.scale;
        if (!load_image_texture(
                context,
                src->occlusion_texture.texture->image,
                gltf_dir,
                false,
                &dst->occlusion) &&
            !pb_rhi_texture_create_solid_rgba8(context, white, false, &dst->occlusion)) {
            return false;
        }
    }

    if (src && src->emissive_texture.texture) {
        set_material_uv_transform(dst, PB_GLTF_TEXTURE_EMISSIVE, &src->emissive_texture);
        pb_rhi_texture_destroy(context, &dst->emissive);
        dst->material_data.emissive_factor[0] = src->emissive_factor[0];
        dst->material_data.emissive_factor[1] = src->emissive_factor[1];
        dst->material_data.emissive_factor[2] = src->emissive_factor[2];
        if (!load_image_texture(
                context,
                src->emissive_texture.texture->image,
                gltf_dir,
                true,
                &dst->emissive) &&
            !pb_rhi_texture_create_solid_rgba8(context, black, false, &dst->emissive)) {
            return false;
        }
    } else if (src) {
        dst->material_data.emissive_factor[0] = src->emissive_factor[0];
        dst->material_data.emissive_factor[1] = src->emissive_factor[1];
        dst->material_data.emissive_factor[2] = src->emissive_factor[2];
    }

    return true;
}

static cgltf_accessor *find_attribute(const cgltf_primitive *prim, cgltf_attribute_type type)
{
    for (cgltf_size i = 0; i < prim->attributes_count; ++i) {
        if (prim->attributes[i].type == type) {
            return prim->attributes[i].data;
        }
    }
    return NULL;
}

static bool build_primitive_mesh(
    pb_context *context,
    const cgltf_primitive *prim,
    pb_rhi_mesh *mesh,
    float bounds_min[3],
    float bounds_max[3])
{
    const cgltf_accessor *pos_attr = find_attribute(prim, cgltf_attribute_type_position);
    if (!pos_attr) {
        return false;
    }

    const cgltf_accessor *norm_attr = find_attribute(prim, cgltf_attribute_type_normal);
    const cgltf_accessor *uv_attr = find_attribute(prim, cgltf_attribute_type_texcoord);
    const cgltf_accessor *tan_attr = find_attribute(prim, cgltf_attribute_type_tangent);

    const uint32_t vertex_count = (uint32_t)pos_attr->count;
    pb_pbr_vertex *vertices = calloc(vertex_count, sizeof(*vertices));
    if (!vertices) {
        return false;
    }

    bool has_tangent = tan_attr != NULL;
    bounds_min[0] = bounds_min[1] = bounds_min[2] = INFINITY;
    bounds_max[0] = bounds_max[1] = bounds_max[2] = -INFINITY;

    for (uint32_t i = 0; i < vertex_count; ++i) {
        cgltf_accessor_read_float(pos_attr, i, vertices[i].pos, 3);
        for (uint32_t axis = 0; axis < 3; ++axis) {
            if (vertices[i].pos[axis] < bounds_min[axis]) {
                bounds_min[axis] = vertices[i].pos[axis];
            }
            if (vertices[i].pos[axis] > bounds_max[axis]) {
                bounds_max[axis] = vertices[i].pos[axis];
            }
        }
        if (norm_attr) {
            cgltf_accessor_read_float(norm_attr, i, vertices[i].normal, 3);
        } else {
            vertices[i].normal[0] = 0.0f;
            vertices[i].normal[1] = 1.0f;
            vertices[i].normal[2] = 0.0f;
        }
        if (uv_attr) {
            cgltf_accessor_read_float(uv_attr, i, vertices[i].uv, 2);
        }
        if (tan_attr) {
            cgltf_accessor_read_float(tan_attr, i, vertices[i].tangent, 4);
            has_tangent = true;
        }
    }

    uint32_t index_count = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT32;
    void *indices = NULL;

    if (prim->indices) {
        index_count = (uint32_t)prim->indices->count;
        if (prim->indices->component_type == cgltf_component_type_r_16u) {
            index_type = VK_INDEX_TYPE_UINT16;
            indices = malloc((size_t)index_count * sizeof(uint16_t));
            if (!indices) {
                free(vertices);
                return false;
            }
            for (uint32_t i = 0; i < index_count; ++i) {
                uint32_t value = 0;
                cgltf_accessor_read_uint(prim->indices, i, &value, 1);
                ((uint16_t *)indices)[i] = (uint16_t)value;
            }
        } else {
            indices = malloc((size_t)index_count * sizeof(uint32_t));
            if (!indices) {
                free(vertices);
                return false;
            }
            for (uint32_t i = 0; i < index_count; ++i) {
                uint32_t value = 0;
                cgltf_accessor_read_uint(prim->indices, i, &value, 1);
                ((uint32_t *)indices)[i] = value;
            }
        }
    } else {
        index_count = vertex_count;
        indices = malloc((size_t)index_count * sizeof(uint32_t));
        if (!indices) {
            free(vertices);
            return false;
        }
        for (uint32_t i = 0; i < index_count; ++i) {
            ((uint32_t *)indices)[i] = i;
        }
    }

    if (!has_tangent) {
        pb_pbr_generate_tangents(vertices, vertex_count, indices, index_count, index_type);
    }

    const bool ok = pb_rhi_mesh_create_interleaved(
        context, vertices, vertex_count, indices, index_count, index_type, mesh);

    if (ok) {
        pb_log_info(
            "glTF mesh uploaded: %u vertices, %u indices (%s)",
            vertex_count,
            index_count,
            index_type == VK_INDEX_TYPE_UINT16 ? "uint16" : "uint32");
    }

    free(vertices);
    free(indices);
    return ok;
}

static void node_local_matrix(const cgltf_node *node, pb_mat4 out)
{
    cgltf_float mat[16];
    cgltf_node_transform_local(node, mat);
    memcpy(out, mat, sizeof(mat));
}

static void traverse_node(
    pb_context *context,
    const cgltf_node *node,
    const pb_mat4 parent,
    const char *gltf_dir,
    pb_gltf_draw **draws,
    uint32_t *draw_count,
    uint32_t *draw_capacity,
    const cgltf_data *data)
{
    pb_mat4 local = {0};
    pb_mat4 world = {0};
    pb_mat4 parent_copy = {0};
    memcpy(parent_copy, parent, sizeof(parent_copy));
    node_local_matrix(node, local);
    pb_mat4_mul(parent_copy, local, world);

    if (node->mesh) {
        const cgltf_mesh *mesh = node->mesh;
        for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
            const cgltf_primitive *prim = &mesh->primitives[p];
            if (prim->type != cgltf_primitive_type_triangles) {
                continue;
            }

            if (*draw_count >= *draw_capacity) {
                const uint32_t new_cap = *draw_capacity == 0 ? 8 : *draw_capacity * 2;
                pb_gltf_draw *grown = realloc(*draws, new_cap * sizeof(**draws));
                if (!grown) {
                    return;
                }
                *draws = grown;
                *draw_capacity = new_cap;
            }

            pb_gltf_draw *draw = &(*draws)[*draw_count];
            memset(draw, 0, sizeof(*draw));
            memcpy(draw->world, world, sizeof(draw->world));

            if (prim->material) {
                draw->material_index = (uint32_t)(prim->material - data->materials);
            }

            if (!build_primitive_mesh(context, prim, &draw->mesh, draw->bounds_min, draw->bounds_max)) {
                pb_log_error("Failed to build glTF primitive mesh");
                continue;
            }

            (*draw_count)++;
        }
    }

    for (cgltf_size c = 0; c < node->children_count; ++c) {
        traverse_node(context, node->children[c], world, gltf_dir, draws, draw_count, draw_capacity, data);
    }
}

static bool create_materials(pb_context *context, cgltf_data *data, const char *gltf_dir, pb_gltf_scene *scene)
{
    scene->material_count = data->materials_count > 0 ? (uint32_t)data->materials_count : 1;
    scene->materials = calloc(scene->material_count, sizeof(*scene->materials));
    if (!scene->materials) {
        return false;
    }

    for (uint32_t i = 0; i < scene->material_count; ++i) {
        pb_gltf_material *mat = &scene->materials[i];
        const cgltf_material *src = data->materials_count > 0 ? &data->materials[i] : NULL;

        mat->material_data.light_color[0] = 4.0f;
        mat->material_data.light_color[1] = 4.0f;
        mat->material_data.light_color[2] = 4.0f;
        mat->material_data.light_dir[0] = 0.5f;
        mat->material_data.light_dir[1] = 0.8f;
        mat->material_data.light_dir[2] = 0.4f;
        mat->material_data.albedo_factor[0] = 1.0f;
        mat->material_data.albedo_factor[1] = 1.0f;
        mat->material_data.albedo_factor[2] = 1.0f;
        mat->material_data.metallic_factor = 1.0f;
        mat->material_data.roughness_factor = 1.0f;
        mat->material_data.alpha_cutoff = 0.5f;
        mat->material_data.base_color_alpha = 1.0f;
        mat->material_data.alpha_mode = (float)PB_GLTF_ALPHA_OPAQUE;
        mat->alpha_mode = PB_GLTF_ALPHA_OPAQUE;
        mat->double_sided = false;
        mat->material_data.double_sided = 0.0f;

        if (!load_material_textures(context, src, gltf_dir, mat)) {
            return false;
        }

        pb_rhi_buffer_desc buffer_desc = {
            .size = sizeof(pb_material_ubo),
            .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
        };

        if (!pb_rhi_buffer_create(context, &buffer_desc, &mat->material_buffer)) {
            return false;
        }

        pb_rhi_buffer_upload(context, &mat->material_buffer, &mat->material_data, sizeof(mat->material_data));
    }

    return true;
}

static const cgltf_scene *resolve_gltf_scene(const cgltf_data *data, uint32_t scene_index)
{
    if (data->scenes_count == 0) {
        return NULL;
    }

    if (scene_index == PB_GLTF_SCENE_INDEX_DEFAULT) {
        if (data->scene) {
            return data->scene;
        }
        return &data->scenes[0];
    }

    if (scene_index >= data->scenes_count) {
        return NULL;
    }

    return &data->scenes[scene_index];
}

static bool collect_scene_draws(
    pb_context *context,
    const cgltf_data *data,
    const cgltf_scene *gltf_scene,
    const char *gltf_dir,
    pb_gltf_scene *scene)
{
    if (!gltf_scene) {
        return false;
    }

    pb_mat4 identity = {0};
    pb_mat4_identity(identity);

    uint32_t draw_capacity = 0;
    for (cgltf_size i = 0; i < gltf_scene->nodes_count; ++i) {
        traverse_node(
            context,
            gltf_scene->nodes[i],
            identity,
            gltf_dir,
            &scene->draws,
            &scene->draw_count,
            &draw_capacity,
            data);
    }

    return scene->draw_count > 0;
}

bool pb_gltf_file_scene_count(const char *path, uint32_t *out_count)
{
    if (!path || !out_count) {
        return false;
    }

    cgltf_options options = {0};
    cgltf_data *data = NULL;
    const cgltf_result result = cgltf_parse_file(&options, path, &data);
    if (result != cgltf_result_success || !data) {
        cgltf_free(data);
        return false;
    }

    *out_count = data->scenes_count;
    cgltf_free(data);
    return true;
}

pb_gltf_scene *pb_gltf_scene_create(const pb_gltf_scene_desc *desc)
{
    if (!desc || !desc->context || !desc->path) {
        pb_log_error("Invalid glTF scene description");
        return NULL;
    }

    if (!pb_context_device_ready(desc->context)) {
        pb_log_error("Vulkan device is not initialized");
        return NULL;
    }

    cgltf_options options = {0};
    cgltf_data *data = NULL;
    cgltf_result result = cgltf_parse_file(&options, desc->path, &data);
    if (result != cgltf_result_success || !data) {
        pb_log_error("Failed to parse glTF: %s", desc->path);
        cgltf_free(data);
        return NULL;
    }

    result = cgltf_load_buffers(&options, data, desc->path);
    if (result != cgltf_result_success) {
        pb_log_error("Failed to load glTF buffers: %s", desc->path);
        cgltf_free(data);
        return NULL;
    }

    char gltf_dir[PB_PATH_MAX];
    path_dirname(desc->path, gltf_dir, sizeof(gltf_dir));

    pb_gltf_scene *scene = calloc(1, sizeof(*scene));
    if (!scene) {
        cgltf_free(data);
        return NULL;
    }

    scene->context = desc->context;

    if (!create_materials(desc->context, data, gltf_dir, scene)) {
        pb_gltf_scene_destroy(scene);
        cgltf_free(data);
        return NULL;
    }

    const uint32_t requested = desc->scene_index;
    const cgltf_scene *gltf_scene = resolve_gltf_scene(data, requested);
    bool loaded = false;

    if (gltf_scene) {
        loaded = collect_scene_draws(desc->context, data, gltf_scene, gltf_dir, scene);
        if (loaded) {
            scene->scene_index =
                requested == PB_GLTF_SCENE_INDEX_DEFAULT
                ? (uint32_t)(data->scene ? (size_t)(data->scene - data->scenes) : 0)
                : requested;
        }
    }

    if (!loaded) {
        pb_mat4 identity = {0};
        pb_mat4_identity(identity);

        uint32_t draw_capacity = 0;
        for (cgltf_size i = 0; i < data->nodes_count; ++i) {
            if (data->nodes[i].parent == NULL) {
                traverse_node(
                    desc->context,
                    &data->nodes[i],
                    identity,
                    gltf_dir,
                    &scene->draws,
                    &scene->draw_count,
                    &draw_capacity,
                    data);
            }
        }

        if (scene->draw_count == 0) {
            pb_log_error("glTF file contains no drawable primitives: %s", desc->path);
            pb_gltf_scene_destroy(scene);
            cgltf_free(data);
            return NULL;
        }

        scene->scene_index = PB_GLTF_SCENE_INDEX_DEFAULT;
    }

    cgltf_free(data);

    pb_log_info(
        "Loaded glTF scene %u: %u draws, %u materials",
        scene->scene_index,
        scene->draw_count,
        scene->material_count);
    return scene;
}

void pb_gltf_scene_destroy(pb_gltf_scene *scene)
{
    if (!scene) {
        return;
    }

    pb_context *context = scene->context;

    if (context && pb_context_device_ready(context)) {
        pb_context_wait_device_idle(context);

        for (uint32_t i = 0; i < scene->draw_count; ++i) {
            pb_rhi_mesh_destroy(context, &scene->draws[i].mesh);
        }

        for (uint32_t i = 0; i < scene->material_count; ++i) {
            pb_gltf_material *mat = &scene->materials[i];
            pb_rhi_texture_destroy(context, &mat->albedo);
            pb_rhi_texture_destroy(context, &mat->metallic_roughness);
            pb_rhi_texture_destroy(context, &mat->normal);
            pb_rhi_texture_destroy(context, &mat->occlusion);
            pb_rhi_texture_destroy(context, &mat->emissive);
            pb_rhi_buffer_destroy(context, &mat->material_buffer);
        }
    }

    free(scene->draws);
    free(scene->materials);
    free(scene);
}

uint32_t pb_gltf_scene_index(const pb_gltf_scene *scene)
{
    return scene ? scene->scene_index : 0;
}

uint32_t pb_gltf_scene_draw_count(const pb_gltf_scene *scene)
{
    return scene ? scene->draw_count : 0;
}

uint32_t pb_gltf_scene_material_count(const pb_gltf_scene *scene)
{
    return scene ? scene->material_count : 0;
}

bool pb_gltf_scene_get_draw(const pb_gltf_scene *scene, uint32_t draw_index, pb_gltf_draw_info *out)
{
    if (!scene || !out || draw_index >= scene->draw_count) {
        return false;
    }

    const pb_gltf_draw *draw = &scene->draws[draw_index];
    out->vertex_buffer = pb_rhi_buffer_handle(&draw->mesh.vertices);
    out->index_buffer = pb_rhi_buffer_handle(&draw->mesh.indices);
    out->index_count = draw->mesh.index_count;
    out->index_type = draw->mesh.index_type;
    out->material_index = draw->material_index;
    memcpy(out->world, draw->world, sizeof(out->world));
    return out->vertex_buffer != VK_NULL_HANDLE && out->index_buffer != VK_NULL_HANDLE && out->index_count > 0;
}

bool pb_gltf_scene_material_factors(
    const pb_gltf_scene *scene,
    uint32_t material_index,
    pb_gltf_material_factors *out)
{
    if (!scene || !out || material_index >= scene->material_count) {
        return false;
    }

    const pb_material_ubo *material = &scene->materials[material_index].material_data;
    out->albedo_factor[0] = material->albedo_factor[0];
    out->albedo_factor[1] = material->albedo_factor[1];
    out->albedo_factor[2] = material->albedo_factor[2];
    out->metallic_factor = material->metallic_factor;
    out->roughness_factor = material->roughness_factor;
    return true;
}

bool pb_gltf_scene_material_uv_transform(
    const pb_gltf_scene *scene,
    uint32_t material_index,
    pb_gltf_texture_map map,
    pb_gltf_uv_transform *out)
{
    if (!scene || !out || material_index >= scene->material_count || map > PB_GLTF_TEXTURE_EMISSIVE) {
        return false;
    }

    const pb_material_ubo *material = &scene->materials[material_index].material_data;
    const float *a = material->uv_transform_a[map];
    const float *b = material->uv_transform_b[map];

    out->offset[0] = a[0];
    out->offset[1] = a[1];
    out->rotation = a[2];
    out->scale[0] = b[0];
    out->scale[1] = b[1];
    out->enabled = a[3] > 0.5f;
    return true;
}

bool pb_gltf_scene_material_info(
    const pb_gltf_scene *scene,
    uint32_t material_index,
    pb_gltf_material_info *out)
{
    if (!scene || !out || material_index >= scene->material_count) {
        return false;
    }

    const pb_gltf_material *material = &scene->materials[material_index];
    out->alpha_mode = material->alpha_mode;
    out->alpha_cutoff = material->material_data.alpha_cutoff;
    out->base_color_alpha = material->material_data.base_color_alpha;
    out->double_sided = material->double_sided;
    return true;
}
