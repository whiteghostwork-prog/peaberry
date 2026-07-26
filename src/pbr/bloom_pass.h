/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_BLOOM_PASS_H
#define PEABERRY_BLOOM_PASS_H

#include "peaberry/peaberry_gltf.h"

#include <volk.h>

/* Internal accessors for the post pass: it needs the bloom result's view +
 * sampler to bind as tonemap.frag's binding 2. Mip 0 of the bloom pyramid holds
 * the final composited bloom. */
VkImageView pb_pbr_bloom_pass_result_view(const pb_pbr_bloom_pass *pass);
VkSampler pb_pbr_bloom_pass_sampler(const pb_pbr_bloom_pass *pass);

#endif
