/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_EXPOSURE_PASS_H
#define PEABERRY_EXPOSURE_PASS_H

#include "peaberry/peaberry_gltf.h"
#include "rhi/buffer.h"

#include <volk.h>

/* Internal accessor for the post pass: it needs the exposure UBO's VkBuffer to
 * bind as tonemap.frag's binding 1 descriptor. The buffer is persistently
 * mapped + host coherent (4-byte float written by exposure_average.comp). */
VkBuffer pb_pbr_exposure_pass_ubo_handle(const pb_pbr_exposure_pass *pass);

#endif
