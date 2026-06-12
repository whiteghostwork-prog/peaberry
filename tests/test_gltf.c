/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_gltf.h"
#include "peaberry/peaberry_vk.h"
#include "test.h"

#ifndef PEABERRY_ASSET_DIR
#define PEABERRY_ASSET_DIR "assets"
#endif

PB_TEST(test_gltf_parse_cube)
{
    char path[512];
    const int path_len = snprintf(path, sizeof(path), "%s/models/test_cube.gltf", PEABERRY_ASSET_DIR);
    PB_ASSERT(path_len >= 0 && path_len < (int)sizeof(path));

    pb_context_desc desc = {
        .app_name = "peaberry gltf test",
        .enable_validation = false,
        .enable_surface = false,
    };

    pb_context *ctx = pb_context_create(&desc);
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    pb_gltf_scene_desc scene_desc = {
        .context = ctx,
        .path = path,
    };

    pb_gltf_scene *scene = pb_gltf_scene_create(&scene_desc);
    if (!scene) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("test_cube.gltf missing or load failed");
    }

    PB_ASSERT(pb_gltf_scene_draw_count(scene) > 0);

    pb_gltf_scene_destroy(scene);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

PB_TEST(test_gltf_alpha_modes)
{
    char path[512];
    const int path_len = snprintf(path, sizeof(path), "%s/models/test_alpha_modes.gltf", PEABERRY_ASSET_DIR);
    PB_ASSERT(path_len >= 0 && path_len < (int)sizeof(path));

    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry gltf alpha test",
            .enable_validation = false,
            .enable_surface = false,
        });
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    pb_gltf_scene *scene = pb_gltf_scene_create(
        &(pb_gltf_scene_desc){
            .context = ctx,
            .path = path,
        });
    if (!scene) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("test_alpha_modes.gltf missing or load failed");
    }

    PB_ASSERT(pb_gltf_scene_material_count(scene) == 3);

    pb_gltf_material_info info = {0};
    PB_ASSERT(pb_gltf_scene_material_info(scene, 0, &info));
    PB_ASSERT(info.alpha_mode == PB_GLTF_ALPHA_OPAQUE);
    PB_ASSERT(info.base_color_alpha == 1.0f);

    PB_ASSERT(pb_gltf_scene_material_info(scene, 1, &info));
    PB_ASSERT(info.alpha_mode == PB_GLTF_ALPHA_MASK);
    PB_ASSERT(info.alpha_cutoff > 0.34f && info.alpha_cutoff < 0.36f);

    PB_ASSERT(pb_gltf_scene_material_info(scene, 2, &info));
    PB_ASSERT(info.alpha_mode == PB_GLTF_ALPHA_BLEND);
    PB_ASSERT(info.base_color_alpha > 0.24f && info.base_color_alpha < 0.26f);

    pb_gltf_scene_destroy(scene);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

PB_TEST(test_gltf_draw_sort_scene)
{
    char path[512];
    const int path_len = snprintf(path, sizeof(path), "%s/models/test_draw_sort.gltf", PEABERRY_ASSET_DIR);
    PB_ASSERT(path_len >= 0 && path_len < (int)sizeof(path));

    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry gltf draw sort test",
            .enable_validation = false,
            .enable_surface = false,
        });
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    pb_gltf_scene *scene = pb_gltf_scene_create(
        &(pb_gltf_scene_desc){
            .context = ctx,
            .path = path,
        });
    if (!scene) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("test_draw_sort.gltf missing or load failed");
    }

    PB_ASSERT(pb_gltf_scene_draw_count(scene) == 3);
    PB_ASSERT(pb_gltf_scene_material_count(scene) == 1);

    pb_gltf_material_info info = {0};
    PB_ASSERT(pb_gltf_scene_material_info(scene, 0, &info));
    PB_ASSERT(info.alpha_mode == PB_GLTF_ALPHA_BLEND);

    pb_gltf_scene_destroy(scene);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

void pb_run_gltf_tests(void)
{
    printf("gltf tests\n");
    PB_RUN_TEST(test_gltf_parse_cube);
    PB_RUN_TEST(test_gltf_alpha_modes);
    PB_RUN_TEST(test_gltf_draw_sort_scene);
}
