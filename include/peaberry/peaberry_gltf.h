/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_GLTF_H
#define PEABERRY_GLTF_H

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_math.h"
#include "peaberry/peaberry_vk.h"

#include <stdint.h>

typedef struct pb_gltf_scene pb_gltf_scene;

enum {
    PB_GLTF_SCENE_INDEX_DEFAULT = UINT32_MAX,
};

typedef struct pb_gltf_scene_desc {
    pb_context *context;
    const char *path;
    uint32_t scene_index;
} pb_gltf_scene_desc;

pb_gltf_scene *pb_gltf_scene_create(const pb_gltf_scene_desc *desc);
void pb_gltf_scene_destroy(pb_gltf_scene *scene);

bool pb_gltf_file_scene_count(const char *path, uint32_t *out_count);

uint32_t pb_gltf_scene_index(const pb_gltf_scene *scene);
uint32_t pb_gltf_scene_draw_count(const pb_gltf_scene *scene);
uint32_t pb_gltf_scene_material_count(const pb_gltf_scene *scene);

typedef struct pb_gltf_draw_info {
    VkBuffer vertex_buffer;
    VkBuffer index_buffer;
    uint32_t index_count;
    VkIndexType index_type;
    uint32_t material_index;
    pb_mat4 world;
} pb_gltf_draw_info;

bool pb_gltf_scene_get_draw(const pb_gltf_scene *scene, uint32_t draw_index, pb_gltf_draw_info *out);

typedef struct pb_gltf_material_factors {
    float albedo_factor[3];
    float metallic_factor;
    float roughness_factor;
} pb_gltf_material_factors;

typedef enum pb_gltf_alpha_mode {
    PB_GLTF_ALPHA_OPAQUE = 0,
    PB_GLTF_ALPHA_MASK = 1,
    PB_GLTF_ALPHA_BLEND = 2,
} pb_gltf_alpha_mode;

typedef struct pb_gltf_material_info {
    pb_gltf_alpha_mode alpha_mode;
    float alpha_cutoff;
    float base_color_alpha;
    bool double_sided;
} pb_gltf_material_info;

bool pb_gltf_scene_material_factors(
    const pb_gltf_scene *scene,
    uint32_t material_index,
    pb_gltf_material_factors *out);

bool pb_gltf_scene_material_info(
    const pb_gltf_scene *scene,
    uint32_t material_index,
    pb_gltf_material_info *out);

typedef enum pb_gltf_texture_map {
    PB_GLTF_TEXTURE_ALBEDO = 0,
    PB_GLTF_TEXTURE_METALLIC_ROUGHNESS = 1,
    PB_GLTF_TEXTURE_NORMAL = 2,
    PB_GLTF_TEXTURE_OCCLUSION = 3,
    PB_GLTF_TEXTURE_EMISSIVE = 4,
} pb_gltf_texture_map;

typedef struct pb_gltf_uv_transform {
    float offset[2];
    float rotation;
    float scale[2];
    bool enabled;
} pb_gltf_uv_transform;

bool pb_gltf_scene_material_uv_transform(
    const pb_gltf_scene *scene,
    uint32_t material_index,
    pb_gltf_texture_map map,
    pb_gltf_uv_transform *out);

typedef struct pb_pbr_forward_pass pb_pbr_forward_pass;

typedef struct pb_pbr_forward_pass_desc {
    pb_context *context;
    VkRenderPass render_pass;
    const char *vert_spv_path;
    const char *frag_spv_path;
    const char *ibl_shader_dir;
    const char *ibl_equirect_hdr_path;
    float exposure;
    pb_gltf_scene *scene;
} pb_pbr_forward_pass_desc;

pb_pbr_forward_pass *pb_pbr_forward_pass_create(const pb_pbr_forward_pass_desc *desc);
void pb_pbr_forward_pass_destroy(pb_pbr_forward_pass *pass);

void pb_pbr_forward_pass_set_scene(pb_pbr_forward_pass *pass, pb_gltf_scene *scene);

bool pb_pbr_forward_pass_scene_is_bound(const pb_pbr_forward_pass *pass);

void pb_pbr_forward_pass_set_camera(
    pb_pbr_forward_pass *pass,
    const pb_mat4 view,
    const pb_mat4 proj,
    const float camera_pos[3]);

void pb_pbr_forward_pass_record(
    pb_pbr_forward_pass *pass,
    VkCommandBuffer cmd,
    VkExtent2D extent,
    const pb_gltf_scene *scene,
    float time_seconds);

#endif
