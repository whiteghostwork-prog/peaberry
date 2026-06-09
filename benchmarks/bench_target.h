/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_BENCH_TARGET_H
#define PEABERRY_BENCH_TARGET_H

#include "peaberry/peaberry.h"
#include "peaberry/peaberry_bench.h"
#include "scenario.h"

#include <stdbool.h>
#include <stdint.h>
#include <volk.h>

typedef struct pb_bench_target pb_bench_target;

bool pb_bench_target_create(
    pb_bench_target **out_target,
    pb_context *context,
    VkExtent2D extent,
    pb_bench_scenario *scenario,
    bool detailed);

void pb_bench_target_destroy(pb_bench_target *target);

bool pb_bench_target_run_frame(pb_bench_target *target, pb_bench_frame *out_frame);

VkRenderPass pb_bench_target_render_pass(const pb_bench_target *target);
VkExtent2D pb_bench_target_extent(const pb_bench_target *target);

#endif
