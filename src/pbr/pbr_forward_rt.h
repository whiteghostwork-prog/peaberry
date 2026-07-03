/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_PBR_FORWARD_RT_H
#define PEABERRY_PBR_FORWARD_RT_H

#include "peaberry/peaberry_gltf.h"

#include <stdbool.h>
#include <volk.h>

struct pb_pbr_forward_pass;

#ifdef PEABERRY_ENABLE_RAYTRACING
typedef struct pb_rt_scene pb_rt_scene;

typedef struct pb_pbr_forward_rt {
    pb_context *context;
    VkShaderModule vert_module;
    VkPipelineLayout pipeline_layout;
    pb_rt_scene *scene;
    VkPipeline pipeline_opaque;
    VkPipeline pipeline_opaque_double;
    VkShaderModule frag_module;
    bool enabled;
    bool available;
} pb_pbr_forward_rt;

bool pb_pbr_forward_rt_create(
    pb_pbr_forward_rt *rt,
    pb_context *context,
    VkShaderModule vert_module,
    VkPipelineLayout pipeline_layout,
    VkRenderPass render_pass,
    VkSampleCountFlagBits samples,
    const char *rt_frag_spv_path);

void pb_pbr_forward_rt_destroy(pb_pbr_forward_rt *rt);

void pb_pbr_forward_rt_set_scene(pb_pbr_forward_rt *rt, pb_gltf_scene *scene);

void pb_pbr_forward_rt_set_enabled(pb_pbr_forward_rt *rt, bool enabled);

bool pb_pbr_forward_rt_available(const pb_pbr_forward_rt *rt);

void pb_pbr_forward_rt_update_scene(pb_pbr_forward_rt *rt, const pb_gltf_scene *scene);

void pb_pbr_forward_rt_write_descriptor(
    pb_pbr_forward_rt *rt,
    VkDescriptorSet set);

VkPipeline pb_pbr_forward_rt_select_opaque_pipeline(
    const pb_pbr_forward_rt *rt,
    VkPipeline fallback,
    VkPipeline fallback_double,
    bool double_sided);

bool pb_pbr_forward_rt_uses_rt_pipeline(
    const pb_pbr_forward_rt *rt,
    VkPipeline pipeline);

uint32_t pb_pbr_forward_rt_extra_binding_count(void);

#else

typedef struct pb_pbr_forward_rt {
    int _unused;
} pb_pbr_forward_rt;

static inline bool pb_pbr_forward_rt_create(
    pb_pbr_forward_rt *rt,
    pb_context *context,
    VkShaderModule vert_module,
    VkPipelineLayout pipeline_layout,
    VkRenderPass render_pass,
    VkSampleCountFlagBits samples,
    const char *rt_frag_spv_path)
{
    (void)rt;
    (void)context;
    (void)vert_module;
    (void)pipeline_layout;
    (void)render_pass;
    (void)samples;
    (void)rt_frag_spv_path;
    return true;
}

static inline void pb_pbr_forward_rt_destroy(pb_pbr_forward_rt *rt)
{
    (void)rt;
}

static inline void pb_pbr_forward_rt_set_scene(pb_pbr_forward_rt *rt, pb_gltf_scene *scene)
{
    (void)rt;
    (void)scene;
}

static inline void pb_pbr_forward_rt_set_enabled(pb_pbr_forward_rt *rt, bool enabled)
{
    (void)rt;
    (void)enabled;
}

static inline bool pb_pbr_forward_rt_available(const pb_pbr_forward_rt *rt)
{
    (void)rt;
    return false;
}

static inline void pb_pbr_forward_rt_update_scene(pb_pbr_forward_rt *rt, const pb_gltf_scene *scene)
{
    (void)rt;
    (void)scene;
}

static inline void pb_pbr_forward_rt_write_descriptor(pb_pbr_forward_rt *rt, VkDescriptorSet set)
{
    (void)rt;
    (void)set;
}

static inline VkPipeline pb_pbr_forward_rt_select_opaque_pipeline(
    const pb_pbr_forward_rt *rt,
    VkPipeline fallback,
    VkPipeline fallback_double,
    bool double_sided)
{
    (void)rt;
    return double_sided ? fallback_double : fallback;
}

static inline bool pb_pbr_forward_rt_uses_rt_pipeline(
    const pb_pbr_forward_rt *rt,
    VkPipeline pipeline)
{
    (void)rt;
    (void)pipeline;
    return false;
}

static inline uint32_t pb_pbr_forward_rt_extra_binding_count(void)
{
    return 0;
}

#endif

#endif
