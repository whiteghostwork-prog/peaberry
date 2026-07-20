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

enum {
    PB_FRAMES_IN_FLIGHT = 2,
};

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
uint32_t pb_gltf_scene_animation_count(const pb_gltf_scene *scene);
float pb_gltf_scene_animation_duration(const pb_gltf_scene *scene, uint32_t clip_index);
bool pb_gltf_scene_update_animation(pb_gltf_scene *scene, uint32_t clip_index, float time_seconds);
uint32_t pb_gltf_scene_skin_count(const pb_gltf_scene *scene);

void pb_gltf_scene_set_frame_slot(pb_gltf_scene *scene, uint32_t slot);

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

/* Lights (Phase 13). Directional occupies slot 0 in the list so the shadow
 * pass can read its direction; point lights occupy slots 1..count-1.
 * color is linear RGB pre-multiplied by intensity, matching the legacy
 * (4,4,4) convention. Point lights opt into shadowing (Phase 14.2) by
 * setting shadow_map_index to a value < PB_POINT_SHADOW_MAX; otherwise
 * it must be UINT32_MAX (unshadowed). */
typedef enum {
    PB_LIGHT_TYPE_DIRECTIONAL = 0,
    PB_LIGHT_TYPE_POINT       = 1,
} pb_light_type;

enum {
    PB_LIGHT_MAX = 8,
    PB_POINT_SHADOW_MAX = 4,  /* max simultaneous shadowed point lights */
};

typedef struct pb_light {
    float position[3];   /* world-space; unused for directional            */
    float range;         /* 0 = no attenuation (point light infinite range) */
    float direction[3];  /* world-space; for directional/spot, ignored for point */
    uint32_t type;       /* pb_light_type */
    float color[3];      /* linear RGB * intensity */
    uint32_t shadow_map_index;  /* point lights: <PB_POINT_SHADOW_MAX to claim a cube shadow slot; UINT32_MAX = unshadowed */
    /* Pad to 64 bytes to match the GPU-side pb_light_ubo / GLSL std140
     * struct-array stride. Future fields go in this pad. */
    float _pad[4];
} pb_light;

typedef struct pb_pbr_forward_pass_desc {
    pb_context *context;
    VkRenderPass render_pass;
    const char *vert_spv_path;
    const char *frag_spv_path;
    const char *ibl_shader_dir;
    const char *ibl_equirect_hdr_path;
    float exposure;
    pb_gltf_scene *scene;
    VkSampleCountFlagBits rasterization_samples;
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

/* Upload a light list. lights[0] must be the directional (its direction drives
 * the shadow map). Pass count=0 (or NULL) to restore the default single
 * directional light so scenes render lit without explicit setup. count is
 * clamped to PB_LIGHT_MAX. */
void pb_pbr_forward_pass_set_lights(
    pb_pbr_forward_pass *pass,
    const pb_light *lights,
    uint32_t count);

void pb_pbr_forward_pass_set_shadows_enabled(pb_pbr_forward_pass *pass, bool enabled);

void pb_pbr_forward_pass_set_shadow_tuning(
    pb_pbr_forward_pass *pass,
    float constant_bias,
    float slope_bias);

void pb_pbr_forward_pass_set_shadow_debug(pb_pbr_forward_pass *pass, bool enabled);

void pb_pbr_forward_pass_set_frustum_culling_enabled(pb_pbr_forward_pass *pass, bool enabled);

uint32_t pb_pbr_forward_pass_last_visible_draw_count(const pb_pbr_forward_pass *pass);

void pb_pbr_forward_pass_set_frame_slot(pb_pbr_forward_pass *pass, uint32_t slot);

void pb_pbr_forward_pass_clear_instancing(pb_pbr_forward_pass *pass);

bool pb_pbr_forward_pass_set_instanced_draw(
    pb_pbr_forward_pass *pass,
    uint32_t draw_index,
    const pb_mat4 *transforms,
    uint32_t instance_count);

uint32_t pb_pbr_forward_pass_instanced_count(const pb_pbr_forward_pass *pass);

void pb_pbr_forward_pass_record_shadow_map(
    pb_pbr_forward_pass *pass,
    VkCommandBuffer cmd,
    VkExtent2D extent,
    const pb_gltf_scene *scene);

void pb_pbr_forward_pass_record(
    pb_pbr_forward_pass *pass,
    VkCommandBuffer cmd,
    VkExtent2D extent,
    const pb_gltf_scene *scene,
    float time_seconds);

/* Phase 15.1 post-processing pass. Tonemaps the HDR scene color produced by
 * the forward pass into the caller's LDR target. The caller is responsible
 * for: rendering the forward pass into an HDR (e.g. R16G16B16A16_SFLOAT)
 * color target, transitioning that target to SHADER_READ_ONLY_OPTIMAL, then
 * recording the post pass inside a render pass targeting the LDR output
 * (swapchain or readback target). Present via an sRGB swapchain format so
 * the sRGB encode happens in the output unit, not the shader. */
typedef struct pb_pbr_post_pass pb_pbr_post_pass;

typedef struct pb_pbr_post_pass_desc {
    pb_context *context;
    VkRenderPass render_pass;
    const char *vert_spv_path;   /* resolves to fullscreen.vert.spv */
    const char *frag_spv_path;   /* resolves to tonemap.frag.spv    */
    float exposure;              /* HDR multiplier applied before ACES */
} pb_pbr_post_pass_desc;

pb_pbr_post_pass *pb_pbr_post_pass_create(const pb_pbr_post_pass_desc *desc);
void pb_pbr_post_pass_destroy(pb_pbr_post_pass *pass);

void pb_pbr_post_pass_record(
    pb_pbr_post_pass *pass,
    VkCommandBuffer cmd,
    VkExtent2D extent,
    VkImageView hdr_scene_view,
    VkSampler hdr_scene_sampler);

#endif
