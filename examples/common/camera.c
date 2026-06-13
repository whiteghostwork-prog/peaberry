/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera.h"

#include "peaberry/peaberry_math.h"

#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Orbit radius from target; unit cube half-extent is 0.5, diagonal ~0.866. */
#define PB_EXAMPLE_CAMERA_MIN_DISTANCE 1.6f
#define PB_EXAMPLE_CAMERA_MAX_DISTANCE 50.0f

struct pb_example_camera {
    float azimuth_rad;
    float elevation_rad;
    float distance;
    float fov_deg;
    float near_plane;
    float far_plane;
    pb_vec3 target;
};

pb_example_camera *pb_example_camera_create(const pb_example_camera_desc *desc)
{
    pb_example_camera *cam = calloc(1, sizeof(*cam));
    if (!cam) {
        return NULL;
    }

    cam->azimuth_rad = desc ? desc->azimuth_rad : 0.0f;
    cam->elevation_rad = desc ? desc->elevation_rad : 0.4f;
    cam->distance = desc && desc->distance > 0.0f ? desc->distance : 3.0f;
    cam->fov_deg = desc && desc->fov_deg > 0.0f ? desc->fov_deg : 45.0f;
    cam->near_plane = desc && desc->near_plane > 0.0f ? desc->near_plane : 0.1f;
    cam->far_plane = desc && desc->far_plane > 0.0f ? desc->far_plane : 100.0f;

    cam->target[0] = 0.0f;
    cam->target[1] = 0.0f;
    cam->target[2] = 0.0f;

    return cam;
}

void pb_example_camera_destroy(pb_example_camera *cam)
{
    free(cam);
}

void pb_example_camera_update(
    pb_example_camera *cam,
    float mouse_dx,
    float mouse_dy,
    float scroll_dy,
    bool left_button_down,
    float dt)
{
    (void)dt;

    if (!cam) {
        return;
    }

    if (left_button_down) {
        const float sensitivity = 0.005f;
        cam->azimuth_rad -= mouse_dx * sensitivity;
        cam->elevation_rad += mouse_dy * sensitivity;

        /* clamp elevation to avoid gimbal lock at poles */
        const float limit = (float)(M_PI / 2.0 - 0.05);
        if (cam->elevation_rad > limit) {
            cam->elevation_rad = limit;
        }
        if (cam->elevation_rad < -limit) {
            cam->elevation_rad = -limit;
        }
    }

    if (scroll_dy != 0.0f) {
        /* Multiplicative dolly only — keep FOV fixed so zoom-in does not blow up perspective. */
        cam->distance *= expf(-scroll_dy * 0.15f);
        if (cam->distance < PB_EXAMPLE_CAMERA_MIN_DISTANCE) {
            cam->distance = PB_EXAMPLE_CAMERA_MIN_DISTANCE;
        }
        if (cam->distance > PB_EXAMPLE_CAMERA_MAX_DISTANCE) {
            cam->distance = PB_EXAMPLE_CAMERA_MAX_DISTANCE;
        }
    }
}

void pb_example_camera_get_view(const pb_example_camera *cam, pb_mat4 view)
{
    if (!cam) {
        pb_mat4_identity(view);
        return;
    }

    /* spherical -> Cartesian eye position */
    const float cos_el = cosf(cam->elevation_rad);
    const pb_vec3 eye = {
        cam->target[0] + cam->distance * cos_el * sinf(cam->azimuth_rad),
        cam->target[1] + cam->distance * sinf(cam->elevation_rad),
        cam->target[2] + cam->distance * cos_el * cosf(cam->azimuth_rad),
    };

    const pb_vec3 up = { 0.0f, 1.0f, 0.0f };
    pb_mat4_look_at(view, eye, cam->target, up);
}

void pb_example_camera_get_proj(const pb_example_camera *cam, float aspect, pb_mat4 proj)
{
    if (!cam) {
        pb_mat4_identity(proj);
        return;
    }

    pb_mat4_perspective(proj, pb_radians(cam->fov_deg), aspect, cam->near_plane, cam->far_plane);
}

void pb_example_camera_get_position(const pb_example_camera *cam, pb_vec3 pos)
{
    if (!cam) {
        pos[0] = pos[1] = pos[2] = 0.0f;
        return;
    }

    const float cos_el = cosf(cam->elevation_rad);
    pos[0] = cam->target[0] + cam->distance * cos_el * sinf(cam->azimuth_rad);
    pos[1] = cam->target[1] + cam->distance * sinf(cam->elevation_rad);
    pos[2] = cam->target[2] + cam->distance * cos_el * cosf(cam->azimuth_rad);
}
