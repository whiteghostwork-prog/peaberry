/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_DRAW_SORT_H
#define PEABERRY_DRAW_SORT_H

#include "peaberry/peaberry_math.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct pb_draw_sort_entry {
    uint32_t draw_index;
    float view_depth;
} pb_draw_sort_entry;

float pb_draw_sort_view_depth(const pb_mat4 view, const pb_mat4 model);

void pb_draw_sort_stable(
    pb_draw_sort_entry *entries,
    uint32_t count,
    bool back_to_front);

#endif
