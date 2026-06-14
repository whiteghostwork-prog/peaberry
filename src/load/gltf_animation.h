/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PEABERRY_GLTF_ANIMATION_H
#define PEABERRY_GLTF_ANIMATION_H

#include "pbr/gltf_scene_internal.h"

typedef struct cgltf_data cgltf_data;

bool pb_gltf_scene_load_nodes_and_animations(const cgltf_data *data, pb_gltf_scene *scene);
void pb_gltf_scene_free_nodes_and_animations(pb_gltf_scene *scene);

void pb_gltf_scene_sync_node_transforms(pb_gltf_scene *scene);
void pb_gltf_scene_sync_draw_worlds(pb_gltf_scene *scene);

bool pb_gltf_scene_apply_animation(pb_gltf_scene *scene, uint32_t clip_index, float time_seconds);

#endif
