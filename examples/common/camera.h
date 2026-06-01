/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Orbit camera helper for examples.  Left-drag rotates, scroll zooms.
 */

#ifndef PEABERRY_EXAMPLE_CAMERA_H
#define PEABERRY_EXAMPLE_CAMERA_H

#include "peaberry/peaberry_math.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct pb_example_camera pb_example_camera;

typedef struct pb_example_camera_desc {
    float azimuth_rad;   /* horizontal angle, radians (default 0) */
    float elevation_rad; /* vertical angle, radians (default ~0.4) */
    float distance;      /* distance from target (default 3.0) */
    float fov_deg;       /* vertical field of view, degrees (default 45) */
    float near_plane;
    float far_plane;
} pb_example_camera_desc;

pb_example_camera *pb_example_camera_create(const pb_example_camera_desc *desc);
void pb_example_camera_destroy(pb_example_camera *cam);

/* Feed GLFW mouse events.  Call once per frame with current state. */
void pb_example_camera_update(
    pb_example_camera *cam,
    float mouse_dx,
    float mouse_dy,
    float scroll_dy,
    bool left_button_down,
    float dt);

/* Retrieve the current view, projection, and camera world position. */
void pb_example_camera_get_view(const pb_example_camera *cam, pb_mat4 view);
void pb_example_camera_get_proj(const pb_example_camera *cam, float aspect, pb_mat4 proj);
void pb_example_camera_get_position(const pb_example_camera *cam, pb_vec3 pos);

#endif
