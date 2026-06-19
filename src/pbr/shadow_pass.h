/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_SHADOW_PASS_H
#define PEABERRY_SHADOW_PASS_H

#include "pbr/gltf_scene_internal.h"
#include "peaberry/peaberry_vk.h"

#include <stdbool.h>
#include <volk.h>

enum {
    PB_SHADOW_MAP_SIZE = 1024,
};

typedef struct pb_shadow_pass pb_shadow_pass;

typedef struct pb_shadow_pass_desc {
    pb_context *context;
    VkPipelineLayout pipeline_layout;
    const char *vert_spv_path;
    const char *frag_spv_path;
} pb_shadow_pass_desc;

bool pb_shadow_pass_create(const pb_shadow_pass_desc *desc, pb_shadow_pass **out_pass);
void pb_shadow_pass_destroy(pb_shadow_pass *pass);

void pb_shadow_scene_bounds(const pb_gltf_scene *scene, float bounds_min[3], float bounds_max[3]);

bool pb_shadow_light_matrices_fit_aabb(
    const float light_dir[3],
    const float bounds_min[3],
    const float bounds_max[3],
    pb_mat4 out_light_view,
    pb_mat4 out_light_proj);

void pb_shadow_pass_record(
    pb_shadow_pass *pass,
    VkCommandBuffer cmd,
    const pb_gltf_scene *scene,
    VkDescriptorSet *material_descriptor_sets,
    uint32_t descriptor_set_count,
    const uint32_t *dynamic_offsets,
    uint32_t dynamic_offset_count);

VkImageView pb_shadow_pass_depth_view(const pb_shadow_pass *pass);
VkSampler pb_shadow_pass_sampler(const pb_shadow_pass *pass);

#endif
