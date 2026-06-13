/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bench_paths.h"
#include "scenario.h"

#include "peaberry/peaberry_gltf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct gltf_state {
    pb_pbr_forward_pass *pass;
    pb_gltf_scene *scene;
} gltf_state;

static uint32_t gltf_total_index_count(const pb_gltf_scene *scene)
{
    uint32_t total = 0;

    for (uint32_t i = 0; i < pb_gltf_scene_draw_count(scene); ++i) {
        pb_gltf_draw_info draw = {0};
        if (pb_gltf_scene_get_draw(scene, i, &draw)) {
            total += draw.index_count;
        }
    }

    return total;
}

static bool gltf_setup(pb_bench_scenario *scenario, pb_context *context, VkRenderPass render_pass, VkExtent2D extent)
{
    const char *model_path = scenario->user_data;
    if (!model_path) {
        return false;
    }

    char vert_spv[512];
    char frag_spv[512];
    if (snprintf(vert_spv, sizeof(vert_spv), "%s/pbr_forward.vert.spv", PEABERRY_SHADER_DIR) >= (int)sizeof(vert_spv) ||
        snprintf(frag_spv, sizeof(frag_spv), "%s/pbr_forward.frag.spv", PEABERRY_SHADER_DIR) >= (int)sizeof(frag_spv)) {
        return false;
    }

    gltf_state *state = calloc(1, sizeof(*state));
    if (!state) {
        return false;
    }

    state->scene = pb_gltf_scene_create(
        &(pb_gltf_scene_desc){
            .context = context,
            .path = model_path,
            .scene_index = PB_GLTF_SCENE_INDEX_DEFAULT,
        });
    if (!state->scene) {
        fprintf(stderr, "failed to load glTF model: %s\n", model_path);
        free(state);
        return false;
    }

    state->pass = pb_pbr_forward_pass_create(
        &(pb_pbr_forward_pass_desc){
            .context = context,
            .render_pass = render_pass,
            .vert_spv_path = vert_spv,
            .frag_spv_path = frag_spv,
            .ibl_shader_dir = PEABERRY_SHADER_DIR,
            .exposure = 1.2f,
            .scene = state->scene,
        });
    if (!state->pass || !pb_pbr_forward_pass_scene_is_bound(state->pass)) {
        pb_gltf_scene_destroy(state->scene);
        free(state);
        return false;
    }

    scenario->user_data = state;
    scenario->info.draw_calls = pb_gltf_scene_draw_count(state->scene);
    scenario->info.index_count = gltf_total_index_count(state->scene);
    scenario->info.material_count = pb_gltf_scene_material_count(state->scene);
    scenario->info.pixels_shaded = extent.width * extent.height;
    return true;
}

static void gltf_teardown(pb_bench_scenario *scenario)
{
    gltf_state *state = scenario ? scenario->user_data : NULL;
    if (!state) {
        return;
    }

    if (state->pass) {
        pb_pbr_forward_pass_destroy(state->pass);
    }
    if (state->scene) {
        pb_gltf_scene_destroy(state->scene);
    }

    free(state);
    scenario->user_data = NULL;
}

static void gltf_record(VkCommandBuffer cmd, VkExtent2D extent, void *user_data)
{
    const gltf_state *state = user_data;
    if (!state || !state->pass || !state->scene) {
        return;
    }

    pb_pbr_forward_pass_record(state->pass, cmd, extent, state->scene, 0.0f);
}

bool pb_bench_scenario_gltf_init(
    pb_bench_scenario *scenario,
    pb_context *context,
    VkExtent2D extent,
    const char *model_path)
{
    if (!scenario || !model_path) {
        return false;
    }

    memset(scenario, 0, sizeof(*scenario));
    scenario->name = "gltf";
    scenario->setup = gltf_setup;
    scenario->teardown = gltf_teardown;
    scenario->record = gltf_record;
    scenario->user_data = (void *)model_path;
    (void)context;
    (void)extent;
    return true;
}
