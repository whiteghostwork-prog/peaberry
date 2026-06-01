/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_LOAD_TANGENT_H
#define PEABERRY_LOAD_TANGENT_H

#include "pbr/vertex.h"

#include <stdint.h>
#include <volk.h>

void pb_pbr_generate_tangents(
    pb_pbr_vertex *vertices,
    uint32_t vertex_count,
    const void *indices,
    uint32_t index_count,
    VkIndexType index_type);

#endif
