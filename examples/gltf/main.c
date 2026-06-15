/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * glTF viewer with orbit camera and forward pass.
 */

#include "camera.h"
#include "peaberry/peaberry.h"
#include "peaberry/peaberry_gltf.h"
#include "wsi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifndef PEABERRY_SHADER_DIR
#define PEABERRY_SHADER_DIR "shaders"
#endif

static char g_vert_spv[512];
static char g_frag_spv[512];

static double g_scroll_y;

static void gltf_scroll_callback(GLFWwindow *window, double xoffset, double yoffset)
{
    (void)window;
    (void)xoffset;
    g_scroll_y += yoffset;
}

static void resource_paths(void)
{
    snprintf(g_vert_spv, sizeof(g_vert_spv), "%s/pbr_forward.vert.spv", PEABERRY_SHADER_DIR);
    snprintf(g_frag_spv, sizeof(g_frag_spv), "%s/pbr_forward.frag.spv", PEABERRY_SHADER_DIR);
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [--stats] [--clip N] <model.gltf|model.glb>\n", prog);
    fprintf(stderr, "  Example: %s assets/models/test_cube.gltf\n", prog);
    fprintf(stderr, "  Khronos sample: scripts/download_damaged_helmet.sh\n");
    fprintf(stderr, "  Rigged sample: scripts/download_rigged_simple.sh\n");
}

int main(int argc, char **argv)
{
    bool show_stats = false;
    int clip_index = 0;
    const char *model_path = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--stats") == 0) {
            show_stats = true;
        } else if (strcmp(argv[i], "--clip") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            clip_index = atoi(argv[++i]);
            if (clip_index < 0) {
                fprintf(stderr, "Invalid clip index: %d\n", clip_index);
                return 1;
            }
        } else if (argv[i][0] != '-' && model_path == NULL) {
            model_path = argv[i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!model_path) {
        print_usage(argv[0]);
        return 1;
    }

    resource_paths();

    pb_context_desc ctx_desc = {
        .app_name = "peaberry gltf",
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
        .title = "peaberry gltf",
        .enable_stats = show_stats,
    };

    pb_example_wsi *wsi = pb_example_wsi_create(&wsi_desc);
    if (!wsi) {
        fprintf(stderr, "Failed to create example window\n");
        pb_context_destroy(ctx);
        return 1;
    }

    pb_gltf_scene *scene = pb_gltf_scene_create(
        &(pb_gltf_scene_desc){
            .context = ctx,
            .path = model_path,
            .scene_index = PB_GLTF_SCENE_INDEX_DEFAULT,
        });
    if (!scene) {
        fprintf(stderr, "Failed to load glTF: %s\n", model_path);
        pb_example_wsi_destroy(wsi);
        pb_context_destroy(ctx);
        return 1;
    }

    pb_pbr_forward_pass *pass = pb_pbr_forward_pass_create(
        &(pb_pbr_forward_pass_desc){
            .context = ctx,
            .render_pass = pb_example_wsi_render_pass(wsi),
            .vert_spv_path = g_vert_spv,
            .frag_spv_path = g_frag_spv,
            .ibl_shader_dir = PEABERRY_SHADER_DIR,
            .ibl_equirect_hdr_path = NULL,
            .exposure = 1.2f,
            .scene = scene,
        });
    if (!pass || !pb_pbr_forward_pass_scene_is_bound(pass)) {
        fprintf(stderr, "Failed to create PBR forward pass (scene material binding failed)\n");
        pb_pbr_forward_pass_destroy(pass);
        pb_gltf_scene_destroy(scene);
        pb_example_wsi_destroy(wsi);
        pb_context_destroy(ctx);
        return 1;
    }

    pb_example_camera *cam = pb_example_camera_create(NULL);
    if (!cam) {
        fprintf(stderr, "Failed to create camera\n");
        pb_pbr_forward_pass_destroy(pass);
        pb_gltf_scene_destroy(scene);
        pb_example_wsi_destroy(wsi);
        pb_context_destroy(ctx);
        return 1;
    }

    GLFWwindow *window = pb_example_wsi_window(wsi);
    glfwSetScrollCallback(window, gltf_scroll_callback);
    double prev_mouse_x = 0.0, prev_mouse_y = 0.0;
    bool first_mouse = true;
    double prev_time = glfwGetTime();

    while (!pb_example_wsi_should_close(wsi)) {
        pb_example_wsi_poll(wsi);

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        /* orbit camera input */
        const double now = glfwGetTime();
        const float dt = (float)(now - prev_time);
        prev_time = now;

        double mx, my;
        glfwGetCursorPos(window, &mx, &my);
        float mouse_dx = 0.0f, mouse_dy = 0.0f;
        if (first_mouse) {
            first_mouse = false;
        } else {
            mouse_dx = (float)(mx - prev_mouse_x);
            mouse_dy = (float)(my - prev_mouse_y);
        }
        prev_mouse_x = mx;
        prev_mouse_y = my;

        /* GLFW scroll callback isn't easily accessible from the WSI helper;
         * we handle scroll via glfwGetKey(GLFW_KEY_UP/DOWN) as a fallback,
         * and document that proper scroll requires extending the WSI helper. */
        /* Mouse wheel zoom; Q/E are slower keyboard fallback. */
        float scroll_dy = (float)g_scroll_y;
        g_scroll_y = 0.0;
        if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {
            scroll_dy += 0.08f;
        }
        if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) {
            scroll_dy -= 0.08f;
        }

        const bool left_down = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

        pb_example_camera_update(cam, mouse_dx, mouse_dy, scroll_dy, left_down, dt);

        if (pb_gltf_scene_animation_count(scene) > 0) {
            const uint32_t clip = (uint32_t)clip_index;
            if (clip < pb_gltf_scene_animation_count(scene)) {
                const float duration = pb_gltf_scene_animation_duration(scene, clip);
                if (duration > 0.0f) {
                    const float anim_time = fmodf((float)now, duration);
                    pb_gltf_scene_update_animation(scene, clip, anim_time);
                }
            }
        }

        if (pb_example_wsi_begin_frame(wsi, 0.02f, 0.02f, 0.025f, 1.0f)) {
            VkExtent2D extent = pb_example_wsi_extent(wsi);
            VkCommandBuffer cmd = pb_example_wsi_command_buffer(wsi);

            pb_mat4 view, proj;
            pb_vec3 cam_pos;
            pb_example_camera_get_view(cam, view);
            const float aspect = extent.height > 0 ? (float)extent.width / (float)extent.height : 1.0f;
            pb_example_camera_get_proj(cam, aspect, proj);
            pb_example_camera_get_position(cam, cam_pos);

            pb_pbr_forward_pass_set_camera(pass, view, proj, cam_pos);
            pb_pbr_forward_pass_record(pass, cmd, extent, scene, 0.0f);

            pb_example_wsi_end_frame(wsi);

            if (show_stats) {
                pb_example_wsi_update_stats_title(wsi);
            }
        }
    }

    pb_example_camera_destroy(cam);
    pb_pbr_forward_pass_destroy(pass);
    pb_gltf_scene_destroy(scene);
    pb_example_wsi_destroy(wsi);
    pb_context_destroy(ctx);
    return 0;
}
