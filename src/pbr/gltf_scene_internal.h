/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_GLTF_SCENE_INTERNAL_H
#define PEABERRY_GLTF_SCENE_INTERNAL_H

#include "peaberry/peaberry_math.h"
#include "rhi/buffer.h"
#include "rhi/mesh.h"
#include "rhi/texture.h"

#include <stdint.h>
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
    float _pad1;
} pb_material_ubo;

typedef struct pb_gltf_material {
    pb_rhi_texture albedo;
    pb_rhi_texture metallic_roughness;
    pb_rhi_texture normal;
    pb_rhi_texture occlusion;
    pb_rhi_texture emissive;
    pb_rhi_buffer material_buffer;
    pb_material_ubo material_data;
} pb_gltf_material;

typedef struct pb_gltf_draw {
    pb_rhi_mesh mesh;
    uint32_t material_index;
    pb_mat4 world;
} pb_gltf_draw;

struct pb_gltf_scene {
    pb_context *context;
    pb_gltf_draw *draws;
    uint32_t draw_count;
    pb_gltf_material *materials;
    uint32_t material_count;
};

#endif
