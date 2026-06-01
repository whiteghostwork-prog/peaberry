/*
 * Copyright 2026 The Peaberry Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_render.h"

#include "wsi.h"

#include <stdio.h>
#include <string.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifndef PEABERRY_SHADER_DIR
#define PEABERRY_SHADER_DIR "shaders"
#endif

#ifndef PEABERRY_ASSET_DIR
#define PEABERRY_ASSET_DIR "assets"
#endif

static char g_vert_spv[512];
static char g_frag_spv[512];
static char g_albedo_tex[512];
static char g_mr_tex[512];
static char g_normal_tex[512];

static void resource_paths(void)
{
    snprintf(g_vert_spv, sizeof(g_vert_spv), "%s/pbr_forward.vert.spv", PEABERRY_SHADER_DIR);
    snprintf(g_frag_spv, sizeof(g_frag_spv), "%s/pbr_forward.frag.spv", PEABERRY_SHADER_DIR);
    snprintf(g_albedo_tex, sizeof(g_albedo_tex), "%s/sphere_albedo.png", PEABERRY_ASSET_DIR);
    snprintf(g_mr_tex, sizeof(g_mr_tex), "%s/sphere_metallic_roughness.png", PEABERRY_ASSET_DIR);
    snprintf(g_normal_tex, sizeof(g_normal_tex), "%s/sphere_normal.png", PEABERRY_ASSET_DIR);
}

int main(void)
{
    resource_paths();

    pb_context_desc ctx_desc = {
        .app_name = "peaberry sphere",
        .enable_validation = true,
        .enable_surface = true,
    };

    pb_context *ctx = pb_context_create(&ctx_desc);
    if (!ctx) {
        fprintf(stderr, "Failed to create peaberry context\n");
        return 1;
    }

    pb_example_wsi_desc wsi_desc = {
        .context = ctx,
        .width = 1280,
        .height = 720,
        .title = "peaberry sphere",
    };

    pb_example_wsi *wsi = pb_example_wsi_create(&wsi_desc);
    if (!wsi) {
        fprintf(stderr, "Failed to create example window\n");
        pb_context_destroy(ctx);
        return 1;
    }

    pb_sphere_pass_desc pass_desc = {
        .context = ctx,
        .render_pass = pb_example_wsi_render_pass(wsi),
        .vert_spv_path = g_vert_spv,
        .frag_spv_path = g_frag_spv,
        .albedo_texture_path = g_albedo_tex,
        .metallic_roughness_texture_path = g_mr_tex,
        .normal_texture_path = g_normal_tex,
        .ibl_shader_dir = PEABERRY_SHADER_DIR,
        .ibl_equirect_hdr_path = NULL,
        .albedo_factor = { 1.0f, 1.0f, 1.0f },
        .metallic_factor = 1.0f,
        .roughness_factor = 1.0f,
        .exposure = 1.2f,
    };

    pb_sphere_pass *sphere = pb_sphere_pass_create(&pass_desc);
    if (!sphere) {
        fprintf(stderr, "Failed to create PBR sphere pass\n");
        pb_example_wsi_destroy(wsi);
        pb_context_destroy(ctx);
        return 1;
    }

    GLFWwindow *window = pb_example_wsi_window(wsi);

    while (!pb_example_wsi_should_close(wsi)) {
        pb_example_wsi_poll(wsi);

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        if (pb_example_wsi_begin_frame(wsi, 0.02f, 0.02f, 0.025f, 1.0f)) {
            pb_sphere_pass_record(
                sphere,
                pb_example_wsi_command_buffer(wsi),
                pb_example_wsi_extent(wsi),
                (float)glfwGetTime());
            pb_example_wsi_end_frame(wsi);
        }
    }

    pb_sphere_pass_destroy(sphere);
    pb_example_wsi_destroy(wsi);
    pb_context_destroy(ctx);
    return 0;
}
