/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_gltf.h"
#include "peaberry/peaberry_math.h"
#include "peaberry/peaberry_vk.h"
#include "pbr/vertex.h"
#include "load/tangent.h"
#include "test.h"

#include <math.h>
#include <volk.h>

static float mat4_translation(const pb_mat4 m, int axis)
{
    return m[3][axis];
}

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
        .scene_index = PB_GLTF_SCENE_INDEX_DEFAULT,
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
            .scene_index = PB_GLTF_SCENE_INDEX_DEFAULT,
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
    PB_ASSERT(!info.double_sided);

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
            .scene_index = PB_GLTF_SCENE_INDEX_DEFAULT,
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
    PB_ASSERT(info.double_sided);

    pb_gltf_scene_destroy(scene);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

PB_TEST(test_gltf_double_sided)
{
    char path[512];
    const int path_len = snprintf(path, sizeof(path), "%s/models/test_double_sided.gltf", PEABERRY_ASSET_DIR);
    PB_ASSERT(path_len >= 0 && path_len < (int)sizeof(path));

    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry gltf double sided test",
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
            .scene_index = PB_GLTF_SCENE_INDEX_DEFAULT,
        });
    if (!scene) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("test_double_sided.gltf missing or load failed");
    }

    PB_ASSERT(pb_gltf_scene_material_count(scene) == 2);

    pb_gltf_material_info info = {0};
    PB_ASSERT(pb_gltf_scene_material_info(scene, 0, &info));
    PB_ASSERT(!info.double_sided);

    PB_ASSERT(pb_gltf_scene_material_info(scene, 1, &info));
    PB_ASSERT(info.double_sided);

    pb_gltf_scene_destroy(scene);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

PB_TEST(test_gltf_hierarchy)
{
    char path[512];
    const int path_len = snprintf(path, sizeof(path), "%s/models/test_hierarchy.gltf", PEABERRY_ASSET_DIR);
    PB_ASSERT(path_len >= 0 && path_len < (int)sizeof(path));

    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry gltf hierarchy test",
            .enable_validation = false,
            .enable_surface = false,
        });
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    uint32_t scene_count = 0;
    PB_ASSERT(pb_gltf_file_scene_count(path, &scene_count));
    PB_ASSERT(scene_count == 2);

    pb_gltf_scene *default_scene = pb_gltf_scene_create(
        &(pb_gltf_scene_desc){
            .context = ctx,
            .path = path,
            .scene_index = PB_GLTF_SCENE_INDEX_DEFAULT,
        });
    if (!default_scene) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("test_hierarchy.gltf missing or load failed");
    }

    PB_ASSERT(pb_gltf_scene_index(default_scene) == 1);
    PB_ASSERT(pb_gltf_scene_draw_count(default_scene) == 1);

    pb_gltf_draw_info draw = {0};
    PB_ASSERT(pb_gltf_scene_get_draw(default_scene, 0, &draw));
    PB_ASSERT(fabsf(mat4_translation(draw.world, 0) - 0.0f) < 1e-4f);

    pb_gltf_scene_destroy(default_scene);

    pb_gltf_scene *multi_root = pb_gltf_scene_create(
        &(pb_gltf_scene_desc){
            .context = ctx,
            .path = path,
            .scene_index = 0,
        });
    PB_ASSERT(multi_root != NULL);
    PB_ASSERT(pb_gltf_scene_index(multi_root) == 0);
    PB_ASSERT(pb_gltf_scene_draw_count(multi_root) == 2);

    PB_ASSERT(pb_gltf_scene_get_draw(multi_root, 0, &draw));
    PB_ASSERT(fabsf(mat4_translation(draw.world, 0) - 0.0f) < 1e-4f);
    PB_ASSERT(pb_gltf_scene_get_draw(multi_root, 1, &draw));
    PB_ASSERT(fabsf(mat4_translation(draw.world, 0) - 10.0f) < 1e-4f);

    pb_gltf_scene_destroy(multi_root);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

PB_TEST(test_gltf_texture_transform)
{
    char path[512];
    const int path_len =
        snprintf(path, sizeof(path), "%s/models/test_texture_transform.gltf", PEABERRY_ASSET_DIR);
    PB_ASSERT(path_len >= 0 && path_len < (int)sizeof(path));

    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry gltf texture transform test",
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
            .scene_index = PB_GLTF_SCENE_INDEX_DEFAULT,
        });
    if (!scene) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("test_texture_transform.gltf missing or load failed");
    }

    PB_ASSERT(pb_gltf_scene_material_count(scene) == 1);

    pb_gltf_uv_transform transform = {0};
    PB_ASSERT(pb_gltf_scene_material_uv_transform(
        scene, 0, PB_GLTF_TEXTURE_ALBEDO, &transform));
    PB_ASSERT(transform.enabled);
    PB_ASSERT(fabsf(transform.offset[0] - 0.25f) < 1e-4f);
    PB_ASSERT(fabsf(transform.offset[1] - 0.25f) < 1e-4f);
    PB_ASSERT(fabsf(transform.scale[0] - 0.5f) < 1e-4f);
    PB_ASSERT(fabsf(transform.scale[1] - 0.5f) < 1e-4f);
    PB_ASSERT(fabsf(transform.rotation) < 1e-4f);

    PB_ASSERT(pb_gltf_scene_material_uv_transform(
        scene, 0, PB_GLTF_TEXTURE_METALLIC_ROUGHNESS, &transform));
    PB_ASSERT(!transform.enabled);

    pb_gltf_scene_destroy(scene);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

PB_TEST(test_gltf_animation)
{
    char path[512];
    const int path_len =
        snprintf(path, sizeof(path), "%s/models/test_animation.gltf", PEABERRY_ASSET_DIR);
    PB_ASSERT(path_len >= 0 && path_len < (int)sizeof(path));

    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry gltf animation test",
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
            .scene_index = PB_GLTF_SCENE_INDEX_DEFAULT,
        });
    if (!scene) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("test_animation.gltf missing or load failed");
    }

    PB_ASSERT(pb_gltf_scene_animation_count(scene) == 1);
    PB_ASSERT(fabsf(pb_gltf_scene_animation_duration(scene, 0) - 2.0f) < 1e-4f);

    PB_ASSERT(pb_gltf_scene_update_animation(scene, 0, 0.0f));
    pb_gltf_draw_info draw = {0};
    PB_ASSERT(pb_gltf_scene_get_draw(scene, 0, &draw));
    PB_ASSERT(fabsf(mat4_translation(draw.world, 0) - (-2.0f)) < 1e-3f);

    PB_ASSERT(pb_gltf_scene_update_animation(scene, 0, 1.0f));
    PB_ASSERT(pb_gltf_scene_get_draw(scene, 0, &draw));
    PB_ASSERT(fabsf(mat4_translation(draw.world, 0) - 0.0f) < 1e-3f);

    PB_ASSERT(pb_gltf_scene_update_animation(scene, 0, 1.5f));
    PB_ASSERT(pb_gltf_scene_get_draw(scene, 0, &draw));
    PB_ASSERT(fabsf(mat4_translation(draw.world, 0) - 1.0f) < 1e-3f);

    pb_gltf_scene_destroy(scene);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

PB_TEST(test_gltf_skinning)
{
    char path[512];
    const int path_len =
        snprintf(path, sizeof(path), "%s/models/RiggedSimple.glb", PEABERRY_ASSET_DIR);
    PB_ASSERT(path_len >= 0 && path_len < (int)sizeof(path));

    pb_context *ctx = pb_context_create(
        &(pb_context_desc){
            .app_name = "peaberry gltf skinning test",
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
            .scene_index = PB_GLTF_SCENE_INDEX_DEFAULT,
        });
    if (!scene) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("RiggedSimple.glb missing or load failed (run scripts/download_rigged_simple.sh)");
    }

    PB_ASSERT(pb_gltf_scene_skin_count(scene) == 1);
    PB_ASSERT(pb_gltf_scene_animation_count(scene) >= 1);
    PB_ASSERT(pb_gltf_scene_draw_count(scene) > 0);

    const float duration = pb_gltf_scene_animation_duration(scene, 0);
    PB_ASSERT(duration > 0.0f);

    PB_ASSERT(pb_gltf_scene_update_animation(scene, 0, 0.0f));
    PB_ASSERT(pb_gltf_scene_update_animation(scene, 0, duration * 0.5f));
    PB_ASSERT(pb_gltf_scene_update_animation(scene, 0, duration));

    pb_gltf_scene_destroy(scene);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

/* Phase 9.6: the mikktspace generator must produce an orthonormal tangent
 * basis whose handedness is recoverable from tangent.w. Build a flat XY quad
 * with UV.u mapped to +X and UV.v to +Y; the tangent should align with +X and
 * the handedness sign should reconstruct the +Y bitangent via cross(N, T)*w. */
PB_TEST(test_gltf_generated_tangents)
{
    pb_pbr_vertex verts[4] = {0};
    /* Counter-clockwise quad in the XY plane, facing +Z. */
    verts[0].pos[0] = -1.0f; verts[0].pos[1] = -1.0f; verts[0].normal[2] = 1.0f;
    verts[0].uv[0] = 0.0f;   verts[0].uv[1] = 0.0f;
    verts[1].pos[0] =  1.0f; verts[1].pos[1] = -1.0f; verts[1].normal[2] = 1.0f;
    verts[1].uv[0] = 1.0f;   verts[1].uv[1] = 0.0f;
    verts[2].pos[0] =  1.0f; verts[2].pos[1] =  1.0f; verts[2].normal[2] = 1.0f;
    verts[2].uv[0] = 1.0f;   verts[2].uv[1] = 1.0f;
    verts[3].pos[0] = -1.0f; verts[3].pos[1] =  1.0f; verts[3].normal[2] = 1.0f;
    verts[3].uv[0] = 0.0f;   verts[3].uv[1] = 1.0f;

    const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};

    pb_pbr_generate_tangents(verts, 4, indices, 6, VK_INDEX_TYPE_UINT16);

    for (int i = 0; i < 4; ++i) {
        const float *t = verts[i].tangent;
        const float *n = verts[i].normal;
        const float tlen = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        PB_ASSERT_FLOAT_EQ(tlen, 1.0f, 1e-4f);                 /* unit tangent   */
        const float tdotn = t[0] * n[0] + t[1] * n[1] + t[2] * n[2];
        PB_ASSERT_FLOAT_EQ(tdotn, 0.0f, 1e-4f);                /* T perpendicular to N */
        PB_ASSERT_FLOAT_EQ(t[3] * t[3], 1.0f, 1e-4f);          /* w is +/-1      */
        /* Tangent should follow +U, which is +X for this layout. */
        PB_ASSERT(t[0] > 0.9f);
        PB_ASSERT_FLOAT_EQ(t[1], 0.0f, 1e-4f);

        /* Reconstructed bitangent = cross(N, T) * w must point along +Y. */
        const float bx = n[1] * t[2] - n[2] * t[1];
        const float by = n[2] * t[0] - n[0] * t[2];
        const float bz = n[0] * t[1] - n[1] * t[0];
        PB_ASSERT(by * t[3] > 0.9f);
        PB_ASSERT_FLOAT_EQ(bx * t[3], 0.0f, 1e-4f);
        PB_ASSERT_FLOAT_EQ(bz * t[3], 0.0f, 1e-4f);
    }

    PB_TEST_PASS();
}

void pb_run_gltf_tests(void)
{
    printf("gltf tests\n");
    PB_RUN_TEST(test_gltf_parse_cube);
    PB_RUN_TEST(test_gltf_alpha_modes);
    PB_RUN_TEST(test_gltf_draw_sort_scene);
    PB_RUN_TEST(test_gltf_double_sided);
    PB_RUN_TEST(test_gltf_hierarchy);
    PB_RUN_TEST(test_gltf_texture_transform);
    PB_RUN_TEST(test_gltf_animation);
    PB_RUN_TEST(test_gltf_skinning);
    PB_RUN_TEST(test_gltf_generated_tangents);
}
