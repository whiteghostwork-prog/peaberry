/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_POINT_SHADOW_PASS_H
#define PEABERRY_POINT_SHADOW_PASS_H

#include "pbr/gltf_scene_internal.h"
#include "peaberry/peaberry_vk.h"

#include <stdbool.h>
#include <volk.h>

/* Phase 14.2: up to PB_POINT_SHADOW_MAX cube depth textures for shadowing
 * point lights. Each shadowed point light claims one slot (via its
 * pb_light.shadow_map_index) and gets a 512x512x6-face cube depth texture
 * rendered every frame. The forward shader samples these as samplerCubeShadow
 * to attenuate the point light's contribution where geometry occludes it. */
typedef struct pb_point_shadow_pass pb_point_shadow_pass;

typedef struct pb_point_shadow_pass_desc {
    pb_context *context;
    VkPipelineLayout pipeline_layout;       /* shared with the forward pass */
    const char *vert_spv_path;              /* point_shadow_depth.vert.spv */
    const char *frag_spv_path;              /* point_shadow_depth.frag.spv */
} pb_point_shadow_pass_desc;

bool pb_point_shadow_pass_create(const pb_point_shadow_pass_desc *desc, pb_point_shadow_pass **out_pass);
void pb_point_shadow_pass_destroy(pb_point_shadow_pass *pass);

/* Render the 6 cube faces for one shadowed point light into slot. Walks the
 * scene's draws with the same skinning/instancing/double-sided handling as
 * the directional shadow pass. */
void pb_point_shadow_pass_record(
    pb_point_shadow_pass *pass,
    VkCommandBuffer cmd,
    const pb_gltf_scene *scene,
    uint32_t slot,
    const float light_pos[3],
    float light_range,
    VkDescriptorSet *material_descriptor_sets,
    uint32_t descriptor_set_count,
    const uint32_t *dynamic_offsets,
    uint32_t dynamic_offset_count,
    uint32_t instanced_draw_index,
    uint32_t instanced_count);

/* Accessors for descriptor binding. point_shadow_views[i] is the cube image
 * view for slot i; sampler is the shared compare sampler. */
const VkImageView *pb_point_shadow_pass_views(const pb_point_shadow_pass *pass);
VkSampler pb_point_shadow_pass_sampler(const pb_point_shadow_pass *pass);

/* Per-face UBO descriptor info (binding 15). The forward pass binds this as a
 * dynamic uniform buffer; the cube pass writes the per-face view-proj + light
 * data into it during record. Returns the underlying VkBuffer + slot offset
 * for the current frame, plus the UBO size. */
VkBuffer pb_point_shadow_pass_frame_buffer(const pb_point_shadow_pass *pass);
VkDeviceSize pb_point_shadow_pass_frame_slot_offset(const pb_point_shadow_pass *pass);
uint32_t pb_point_shadow_pass_frame_ubo_size(void);
void pb_point_shadow_pass_set_frame_slot(pb_point_shadow_pass *pass, uint32_t slot);

#endif
