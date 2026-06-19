/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_GLTF_SCENE_INTERNAL_H
#define PEABERRY_GLTF_SCENE_INTERNAL_H

#include "peaberry/peaberry_gltf.h"
#include "peaberry/peaberry_math.h"
#include "rhi/buffer.h"
#include "rhi/mesh.h"
#include "rhi/texture.h"

#include <stdint.h>
#include <stdalign.h>
#include <volk.h>

typedef struct pb_material_ubo {
    float light_dir[3];
    float _pad0;
    float albedo_factor[3];
    float metallic_factor;
    float light_color[3];
    float roughness_factor;
    float occlusion_strength;
    float emissive_factor[3];
    float alpha_cutoff;
    float base_color_alpha;
    float alpha_mode;
    float double_sided;
    float uv_transform_a[5][4];
    float uv_transform_b[5][4];
} pb_material_ubo;

typedef struct pb_gltf_material {
    pb_gltf_alpha_mode alpha_mode;
    bool double_sided;
    pb_rhi_texture albedo;
    pb_rhi_texture metallic_roughness;
    pb_rhi_texture normal;
    pb_rhi_texture occlusion;
    pb_rhi_texture emissive;
    pb_rhi_buffer material_buffer;
    pb_material_ubo material_data;
} pb_gltf_material;

enum {
    PB_GLTF_SKIN_JOINTS_MAX = 128,
    PB_GLTF_NO_SKIN = UINT32_MAX,
};

typedef struct pb_gltf_skin {
    uint32_t *joint_nodes;
    uint32_t joint_count;
    alignas(16) pb_mat4 *inverse_bind;
} pb_gltf_skin;

typedef struct pb_gltf_draw {
    pb_rhi_mesh mesh;
    uint32_t material_index;
    uint32_t node_index;
    uint32_t skin_index;
    alignas(16) pb_mat4 world;
    float bounds_min[3];
    float bounds_max[3];
} pb_gltf_draw;

typedef enum pb_gltf_anim_path {
    PB_GLTF_ANIM_PATH_TRANSLATION = 0,
    PB_GLTF_ANIM_PATH_ROTATION = 1,
    PB_GLTF_ANIM_PATH_SCALE = 2,
    PB_GLTF_ANIM_PATH_WEIGHTS = 3,
} pb_gltf_anim_path;

typedef enum pb_gltf_interpolation {
    PB_GLTF_INTERPOLATION_LINEAR = 0,
    PB_GLTF_INTERPOLATION_STEP = 1,
    PB_GLTF_INTERPOLATION_CUBIC = 2,
} pb_gltf_interpolation;

typedef struct pb_gltf_anim_sampler {
    float *input;
    uint32_t input_count;
    float *output;
    uint32_t output_count;
    uint32_t output_components;
    pb_gltf_interpolation interpolation;
} pb_gltf_anim_sampler;

typedef struct pb_gltf_anim_channel {
    uint32_t target_node;
    uint32_t sampler_index;
    pb_gltf_anim_path path;
} pb_gltf_anim_channel;

typedef struct pb_gltf_animation {
    char *name;
    float duration;
    pb_gltf_anim_sampler *samplers;
    uint32_t sampler_count;
    pb_gltf_anim_channel *channels;
    uint32_t channel_count;
} pb_gltf_animation;

typedef struct pb_gltf_node {
    uint32_t parent;
    float translation[3];
    float rotation[4];
    float scale[3];
    bool has_translation;
    bool has_rotation;
    bool has_scale;
    bool has_matrix;
    alignas(16) pb_mat4 bind_matrix;
    float anim_translation[3];
    float anim_rotation[4];
    float anim_scale[3];
    bool anim_translation_active;
    bool anim_rotation_active;
    bool anim_scale_active;
    alignas(16) pb_mat4 local;
    alignas(16) pb_mat4 world;
} pb_gltf_node;

struct pb_gltf_scene {
    pb_context *context;
    pb_gltf_draw *draws;
    uint32_t draw_count;
    pb_gltf_material *materials;
    uint32_t material_count;
    uint32_t scene_index;
    pb_gltf_node *nodes;
    uint32_t node_count;
    pb_gltf_animation *animations;
    uint32_t animation_count;
    pb_gltf_skin *skins;
    uint32_t skin_count;
    alignas(16) float *skin_palette;
    size_t skin_palette_bytes;
    pb_rhi_buffer skin_palette_buffer;
    uint32_t frame_slot;
};

typedef struct pb_pbr_push_constants {
    pb_mat4 model;
    uint32_t skinned;
    uint32_t palette_base;
    uint32_t instanced;
    uint32_t instance_base;
} pb_pbr_push_constants;

#endif
