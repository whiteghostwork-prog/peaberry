/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_BENCH_SCENARIO_H
#define PEABERRY_BENCH_SCENARIO_H

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_bench.h"

#include <stdbool.h>
#include <stdint.h>
#include <volk.h>

typedef struct pb_bench_scenario_info {
    uint32_t draw_calls;
    uint32_t index_count;
    uint32_t material_count;
    uint32_t pixels_shaded;
} pb_bench_scenario_info;

typedef void (*pb_bench_record_fn)(VkCommandBuffer cmd, VkExtent2D extent, void *user_data);

typedef struct pb_bench_scenario {
    const char *name;
    bool (*setup)(struct pb_bench_scenario *scenario, pb_context *context, VkRenderPass render_pass, VkExtent2D extent);
    void (*teardown)(struct pb_bench_scenario *scenario);
    pb_bench_record_fn record;
    void *user_data;
    pb_bench_scenario_info info;
} pb_bench_scenario;

bool pb_bench_scenario_clear_init(pb_bench_scenario *scenario, pb_context *context, VkExtent2D extent);
bool pb_bench_scenario_sphere_init(pb_bench_scenario *scenario, pb_context *context, VkExtent2D extent);
bool pb_bench_scenario_gltf_init(
    pb_bench_scenario *scenario,
    pb_context *context,
    VkExtent2D extent,
    const char *model_path);

#endif
