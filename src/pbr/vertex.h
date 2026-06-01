/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_PBR_VERTEX_H
#define PEABERRY_PBR_VERTEX_H

#include <stdint.h>

typedef struct pb_pbr_vertex {
    float pos[3];
    float normal[3];
    float uv[2];
    float tangent[4];
} pb_pbr_vertex;

enum { PB_PBR_VERTEX_STRIDE_FLOATS = 12 };

#endif
