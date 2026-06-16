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

#ifndef PEABERRY_MATH_H
#define PEABERRY_MATH_H

typedef float pb_mat4[4][4];
typedef float pb_vec3[3];

void pb_mat4_identity(pb_mat4 m);
void pb_mat4_mul(pb_mat4 a, pb_mat4 b, pb_mat4 out);
void pb_mat4_perspective(pb_mat4 m, float fovy_rad, float aspect, float near_z, float far_z);
void pb_mat4_look_at(
    pb_mat4 m,
    const pb_vec3 eye,
    const pb_vec3 center,
    const pb_vec3 up);
void pb_mat4_rotate_y(pb_mat4 m, float angle_rad, pb_mat4 out);
void pb_mat4_ortho(
    pb_mat4 m,
    float left,
    float right,
    float bottom,
    float top,
    float near_z,
    float far_z);

void pb_vec3_normalize(pb_vec3 v, pb_vec3 out);

float pb_radians(float degrees);

#endif
