/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_FRUSTUM_CULL_H
#define PEABERRY_FRUSTUM_CULL_H

#include "peaberry/peaberry_math.h"

#include <stdbool.h>

typedef struct pb_frustum {
    float planes[6][4];
} pb_frustum;

void pb_frustum_from_view_proj(const pb_mat4 view, const pb_mat4 proj, pb_frustum *out);

bool pb_frustum_intersects_bounds(
    const pb_frustum *frustum,
    const pb_mat4 world,
    const float bounds_min[3],
    const float bounds_max[3]);

#endif
