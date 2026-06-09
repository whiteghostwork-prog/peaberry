/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "scenario.h"

#include <string.h>

static bool clear_setup(pb_bench_scenario *scenario, pb_context *context, VkRenderPass render_pass, VkExtent2D extent)
{
    (void)context;
    (void)render_pass;

    scenario->info.draw_calls = 0;
    scenario->info.index_count = 0;
    scenario->info.material_count = 0;
    scenario->info.pixels_shaded = extent.width * extent.height;
    return true;
}

static void clear_teardown(pb_bench_scenario *scenario)
{
    (void)scenario;
}

static void clear_record(VkCommandBuffer cmd, VkExtent2D extent, void *user_data)
{
    (void)cmd;
    (void)extent;
    (void)user_data;
}

bool pb_bench_scenario_clear_init(pb_bench_scenario *scenario, pb_context *context, VkExtent2D extent)
{
    if (!scenario) {
        return false;
    }

    memset(scenario, 0, sizeof(*scenario));
    scenario->name = "clear";
    scenario->setup = clear_setup;
    scenario->teardown = clear_teardown;
    scenario->record = clear_record;
    (void)context;
    (void)extent;
    return true;
}
