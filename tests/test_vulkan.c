/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_vk.h"
#include "pbr/ibl.h"
#include "test.h"

#ifndef PEABERRY_SHADER_DIR
#define PEABERRY_SHADER_DIR "shaders"
#endif

PB_TEST(test_context_create_destroy)
{
    pb_context_desc desc = {
        .app_name = "peaberry test",
        .enable_validation = false,
        .enable_surface = false,
    };

    pb_context *ctx = pb_context_create(&desc);
    PB_ASSERT(ctx != NULL);
    PB_ASSERT(pb_context_instance(ctx) != VK_NULL_HANDLE);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

PB_TEST(test_headless_device)
{
    pb_context_desc desc = {
        .app_name = "peaberry test",
        .enable_validation = false,
        .enable_surface = false,
    };

    pb_context *ctx = pb_context_create(&desc);
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    PB_ASSERT(pb_context_device_ready(ctx));
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

PB_TEST(test_ibl_environment_bake)
{
    pb_context_desc desc = {
        .app_name = "peaberry test",
        .enable_validation = false,
        .enable_surface = false,
    };

    pb_context *ctx = pb_context_create(&desc);
    PB_ASSERT(ctx != NULL);

    if (!pb_context_init_headless_device(ctx)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("no Vulkan device");
    }

    pb_ibl_environment env = {0};
    const pb_ibl_environment_desc ibl_desc = {
        .context = ctx,
        .equirect_hdr_path = NULL,
        .shader_dir = PEABERRY_SHADER_DIR,
    };

    if (!pb_ibl_environment_create(&ibl_desc, &env)) {
        pb_context_destroy(ctx);
        PB_TEST_SKIP("IBL bake unavailable");
    }

    PB_ASSERT(env.brdf_lut.image != VK_NULL_HANDLE);
    PB_ASSERT(env.irradiance.image != VK_NULL_HANDLE);
    PB_ASSERT(env.prefilter.image != VK_NULL_HANDLE);
    PB_ASSERT(env.prefilter_max_lod > 0.0f);

    pb_ibl_environment_destroy(ctx, &env);
    pb_context_destroy(ctx);
    PB_TEST_PASS();
}

void pb_run_vulkan_tests(void)
{
    printf("vulkan tests\n");
    PB_RUN_TEST(test_context_create_destroy);
    PB_RUN_TEST(test_headless_device);
    PB_RUN_TEST(test_ibl_environment_bake);
}
