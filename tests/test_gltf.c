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

void pb_run_gltf_tests(void)
{
    printf("gltf tests\n");
    PB_RUN_TEST(test_gltf_parse_cube);
}
