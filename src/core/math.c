/*
 * Copyright 2026 The Peaberry Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "peaberry/peaberry_math.h"

#include <cglm/cglm.h>

void pb_mat4_identity(pb_mat4 m)
{
    glm_mat4_identity(m);
}

void pb_mat4_mul(pb_mat4 a, pb_mat4 b, pb_mat4 out)
{
    glm_mat4_mul(a, b, out);
}

void pb_mat4_perspective(pb_mat4 m, float fovy_rad, float aspect, float near_z, float far_z)
{
    glm_perspective(fovy_rad, aspect, near_z, far_z, m);
}

void pb_mat4_look_at(
    pb_mat4 m,
    const pb_vec3 eye,
    const pb_vec3 center,
    const pb_vec3 up)
{
    glm_lookat((vec3){eye[0], eye[1], eye[2]}, (vec3){center[0], center[1], center[2]}, (vec3){up[0], up[1], up[2]}, m);
}

void pb_mat4_rotate_y(pb_mat4 m, float angle_rad, pb_mat4 out)
{
    mat4 temp;
    glm_mat4_copy(m, temp);
    glm_rotate_y(temp, angle_rad, out);
}

float pb_radians(float degrees)
{
    return glm_rad(degrees);
}
