/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bench_paths.h"
#include "scenario.h"

#include "peaberry/peaberry_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SPHERE_SECTORS = 48,
    SPHERE_STACKS = 32,
};

typedef struct sphere_state {
    pb_sphere_pass *pass;
} sphere_state;

static bool sphere_setup(pb_bench_scenario *scenario, pb_context *context, VkRenderPass render_pass, VkExtent2D extent)
{
    char vert_spv[512];
    char frag_spv[512];
    char albedo[512];
    char mr[512];
    char normal[512];

    if (snprintf(vert_spv, sizeof(vert_spv), "%s/pbr_forward.vert.spv", PEABERRY_SHADER_DIR) >= (int)sizeof(vert_spv) ||
        snprintf(frag_spv, sizeof(frag_spv), "%s/pbr_forward.frag.spv", PEABERRY_SHADER_DIR) >= (int)sizeof(frag_spv) ||
        snprintf(albedo, sizeof(albedo), "%s/sphere_albedo.png", PEABERRY_ASSET_DIR) >= (int)sizeof(albedo) ||
        snprintf(mr, sizeof(mr), "%s/sphere_metallic_roughness.png", PEABERRY_ASSET_DIR) >= (int)sizeof(mr) ||
        snprintf(normal, sizeof(normal), "%s/sphere_normal.png", PEABERRY_ASSET_DIR) >= (int)sizeof(normal)) {
        return false;
    }

    sphere_state *state = calloc(1, sizeof(*state));
    if (!state) {
        return false;
    }

    state->pass = pb_sphere_pass_create(
        &(pb_sphere_pass_desc){
            .context = context,
            .render_pass = render_pass,
            .vert_spv_path = vert_spv,
            .frag_spv_path = frag_spv,
            .albedo_texture_path = albedo,
            .metallic_roughness_texture_path = mr,
            .normal_texture_path = normal,
            .ibl_shader_dir = PEABERRY_SHADER_DIR,
            .exposure = 1.2f,
        });
    if (!state->pass) {
        free(state);
        return false;
    }

    scenario->user_data = state;
    scenario->info.draw_calls = 1;
    scenario->info.index_count = SPHERE_STACKS * SPHERE_SECTORS * 6;
    scenario->info.material_count = 1;
    scenario->info.pixels_shaded = extent.width * extent.height;
    return true;
}

static void sphere_teardown(pb_bench_scenario *scenario)
{
    sphere_state *state = scenario ? scenario->user_data : NULL;
    if (!state) {
        return;
    }

    if (state->pass) {
        pb_sphere_pass_destroy(state->pass);
    }

    free(state);
    scenario->user_data = NULL;
}

static void sphere_record(VkCommandBuffer cmd, VkExtent2D extent, void *user_data)
{
    const sphere_state *state = user_data;
    if (!state || !state->pass) {
        return;
    }

    pb_sphere_pass_record(state->pass, cmd, extent, 0.0f);
}

bool pb_bench_scenario_sphere_init(pb_bench_scenario *scenario, pb_context *context, VkExtent2D extent)
{
    if (!scenario) {
        return false;
    }

    memset(scenario, 0, sizeof(*scenario));
    scenario->name = "sphere";
    scenario->setup = sphere_setup;
    scenario->teardown = sphere_teardown;
    scenario->record = sphere_record;
    (void)context;
    (void)extent;
    return true;
}
