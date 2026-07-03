/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_RT_SCENE_H
#define PEABERRY_RT_SCENE_H

#include "pbr/gltf_scene_internal.h"

#include <stdbool.h>
#include <volk.h>

typedef struct pb_rt_scene pb_rt_scene;

pb_rt_scene *pb_rt_scene_create(pb_context *context, const pb_gltf_scene *scene);
void pb_rt_scene_destroy(pb_rt_scene *scene);

bool pb_rt_scene_update(pb_rt_scene *scene, const pb_gltf_scene *gltf_scene);

VkAccelerationStructureKHR pb_rt_scene_tlas(const pb_rt_scene *scene);

#endif
