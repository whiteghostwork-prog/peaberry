/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "load/gltf_animation.h"

#include "core/log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <stdalign.h>

#define CGLM_FORCE_DEPTH_ZERO_TO_ONE
#define CGLM_FORCE_LEFT_HANDED
#include <cglm/cglm.h>

#include "cgltf.h"
#include "rhi/buffer.h"

enum {
    PB_GLTF_NO_PARENT = UINT32_MAX,
};

static void mat4_invert(const pb_mat4 matrix, pb_mat4 out)
{
    glm_mat4_inv(matrix, out);
}

static bool load_skins(const cgltf_data *data, pb_gltf_scene *scene)
{
    scene->skin_count = (uint32_t)data->skins_count;
    if (scene->skin_count == 0) {
        return true;
    }

    scene->skins = calloc(scene->skin_count, sizeof(*scene->skins));
    if (!scene->skins) {
        return false;
    }

    for (uint32_t s = 0; s < scene->skin_count; ++s) {
        const cgltf_skin *src = &data->skins[s];
        pb_gltf_skin *dst = &scene->skins[s];

        dst->joint_count = (uint32_t)src->joints_count;
        if (dst->joint_count == 0 || dst->joint_count > PB_GLTF_SKIN_JOINTS_MAX) {
            return false;
        }

        dst->joint_nodes = calloc(dst->joint_count, sizeof(*dst->joint_nodes));
        dst->inverse_bind = calloc(dst->joint_count, sizeof(*dst->inverse_bind));
        if (!dst->joint_nodes || !dst->inverse_bind) {
            return false;
        }

        for (uint32_t j = 0; j < dst->joint_count; ++j) {
            if (!src->joints[j]) {
                return false;
            }
            const cgltf_size joint_index = cgltf_node_index(data, src->joints[j]);
            if (joint_index >= data->nodes_count) {
                return false;
            }
            dst->joint_nodes[j] = (uint32_t)joint_index;

            if (src->inverse_bind_matrices) {
                if (!cgltf_accessor_read_float(src->inverse_bind_matrices, j, dst->inverse_bind[j][0], 16)) {
                    return false;
                }
            } else {
                for (int r = 0; r < 4; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        dst->inverse_bind[j][r][c] = r == c ? 1.0f : 0.0f;
                    }
                }
            }
        }
    }

    pb_log_info("Loaded glTF skin data: %u skins", scene->skin_count);
    return true;
}

static bool copy_accessor_floats(
    const cgltf_accessor *accessor,
    float **out_values,
    uint32_t *out_count,
    uint32_t components)
{
    if (!accessor || !out_values || !out_count || components == 0) {
        return false;
    }

    const uint32_t count = (uint32_t)accessor->count;
    float *values = calloc((size_t)count * components, sizeof(float));
    if (!values) {
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (!cgltf_accessor_read_float(accessor, i, &values[i * components], components)) {
            free(values);
            return false;
        }
    }

    *out_values = values;
    *out_count = count;
    return true;
}

static void copy_node_bind_pose(const cgltf_node *src, pb_gltf_node *dst)
{
    dst->has_translation = src->has_translation ? true : false;
    dst->has_rotation = src->has_rotation ? true : false;
    dst->has_scale = src->has_scale ? true : false;
    dst->has_matrix = src->has_matrix ? true : false;

    memcpy(dst->translation, src->translation, sizeof(dst->translation));
    memcpy(dst->rotation, src->rotation, sizeof(dst->rotation));
    memcpy(dst->scale, src->scale, sizeof(dst->scale));
    memcpy(dst->bind_matrix, src->matrix, sizeof(dst->bind_matrix));

    if (!dst->has_scale) {
        dst->scale[0] = 1.0f;
        dst->scale[1] = 1.0f;
        dst->scale[2] = 1.0f;
    }
    if (!dst->has_rotation) {
        dst->rotation[0] = 0.0f;
        dst->rotation[1] = 0.0f;
        dst->rotation[2] = 0.0f;
        dst->rotation[3] = 1.0f;
    }
}

static void reset_node_animation_state(pb_gltf_node *node)
{
    node->anim_translation_active = false;
    node->anim_rotation_active = false;
    node->anim_scale_active = false;
    memcpy(node->anim_translation, node->translation, sizeof(node->anim_translation));
    memcpy(node->anim_rotation, node->rotation, sizeof(node->anim_rotation));
    memcpy(node->anim_scale, node->scale, sizeof(node->anim_scale));
}

static void quat_normalize(float q[4])
{
    const float len =
        sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len > 1e-8f) {
        q[0] /= len;
        q[1] /= len;
        q[2] /= len;
        q[3] /= len;
    } else {
        q[0] = 0.0f;
        q[1] = 0.0f;
        q[2] = 0.0f;
        q[3] = 1.0f;
    }
}

static void quat_slerp(const float a[4], const float b[4], float t, float out[4])
{
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float b_adj[4] = { b[0], b[1], b[2], b[3] };

    if (dot < 0.0f) {
        dot = -dot;
        b_adj[0] = -b_adj[0];
        b_adj[1] = -b_adj[1];
        b_adj[2] = -b_adj[2];
        b_adj[3] = -b_adj[3];
    }

    if (dot > 0.9995f) {
        out[0] = a[0] + t * (b_adj[0] - a[0]);
        out[1] = a[1] + t * (b_adj[1] - a[1]);
        out[2] = a[2] + t * (b_adj[2] - a[2]);
        out[3] = a[3] + t * (b_adj[3] - a[3]);
        quat_normalize(out);
        return;
    }

    const float theta = acosf(fminf(fmaxf(dot, -1.0f), 1.0f));
    const float sin_theta = sinf(theta);
    const float w0 = sinf((1.0f - t) * theta) / sin_theta;
    const float w1 = sinf(t * theta) / sin_theta;

    out[0] = a[0] * w0 + b_adj[0] * w1;
    out[1] = a[1] * w0 + b_adj[1] * w1;
    out[2] = a[2] * w0 + b_adj[2] * w1;
    out[3] = a[3] * w0 + b_adj[3] * w1;
}

static void trs_to_mat4(
    const float translation[3],
    const float rotation[4],
    const float scale[3],
    pb_mat4 out)
{
    const float x = rotation[0];
    const float y = rotation[1];
    const float z = rotation[2];
    const float w = rotation[3];

    const float x2 = x + x;
    const float y2 = y + y;
    const float z2 = z + z;
    const float xx = x * x2;
    const float yy = y * y2;
    const float zz = z * z2;
    const float xy = x * y2;
    const float xz = x * z2;
    const float yz = y * z2;
    const float wx = w * x2;
    const float wy = w * y2;
    const float wz = w * z2;

    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r][c] = r == c ? 1.0f : 0.0f;
        }
    }

    out[0][0] = (1.0f - (yy + zz)) * scale[0];
    out[0][1] = (xy + wz) * scale[0];
    out[0][2] = (xz - wy) * scale[0];

    out[1][0] = (xy - wz) * scale[1];
    out[1][1] = (1.0f - (xx + zz)) * scale[1];
    out[1][2] = (yz + wx) * scale[1];

    out[2][0] = (xz + wy) * scale[2];
    out[2][1] = (yz - wx) * scale[2];
    out[2][2] = (1.0f - (xx + yy)) * scale[2];

    out[3][0] = translation[0];
    out[3][1] = translation[1];
    out[3][2] = translation[2];
}

static uint32_t find_keyframe_index(const float *times, uint32_t count, float time)
{
    if (count <= 1) {
        return 0;
    }

    uint32_t lo = 0;
    uint32_t hi = count - 1;
    while (lo + 1 < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (times[mid] <= time) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void sample_vec3(
    const pb_gltf_anim_sampler *sampler,
    float time,
    float out[3])
{
    if (sampler->input_count == 0 || sampler->output_count == 0) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }

    if (time <= sampler->input[0] || sampler->input_count == 1) {
        out[0] = sampler->output[0];
        out[1] = sampler->output[1];
        out[2] = sampler->output[2];
        return;
    }

    const uint32_t last = sampler->input_count - 1;
    if (time >= sampler->input[last]) {
        const float *v = &sampler->output[last * 3];
        out[0] = v[0];
        out[1] = v[1];
        out[2] = v[2];
        return;
    }

    const uint32_t i = find_keyframe_index(sampler->input, sampler->input_count, time);
    const uint32_t j = i + 1;
    const float t0 = sampler->input[i];
    const float t1 = sampler->input[j];
    const float *v0 = &sampler->output[i * 3];
    const float *v1 = &sampler->output[j * 3];

    if (sampler->interpolation == PB_GLTF_INTERPOLATION_STEP) {
        out[0] = v0[0];
        out[1] = v0[1];
        out[2] = v0[2];
        return;
    }

    const float alpha = (time - t0) / fmaxf(t1 - t0, 1e-8f);
    out[0] = v0[0] + (v1[0] - v0[0]) * alpha;
    out[1] = v0[1] + (v1[1] - v0[1]) * alpha;
    out[2] = v0[2] + (v1[2] - v0[2]) * alpha;
}

static void sample_quat(
    const pb_gltf_anim_sampler *sampler,
    float time,
    float out[4])
{
    if (sampler->input_count == 0 || sampler->output_count == 0) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }

    if (time <= sampler->input[0] || sampler->input_count == 1) {
        out[0] = sampler->output[0];
        out[1] = sampler->output[1];
        out[2] = sampler->output[2];
        out[3] = sampler->output[3];
        quat_normalize(out);
        return;
    }

    const uint32_t last = sampler->input_count - 1;
    if (time >= sampler->input[last]) {
        const float *v = &sampler->output[last * 4];
        out[0] = v[0];
        out[1] = v[1];
        out[2] = v[2];
        out[3] = v[3];
        quat_normalize(out);
        return;
    }

    const uint32_t i = find_keyframe_index(sampler->input, sampler->input_count, time);
    const uint32_t j = i + 1;
    const float t0 = sampler->input[i];
    const float t1 = sampler->input[j];
    const float a[4] = {
        sampler->output[i * 4 + 0],
        sampler->output[i * 4 + 1],
        sampler->output[i * 4 + 2],
        sampler->output[i * 4 + 3],
    };
    const float b[4] = {
        sampler->output[j * 4 + 0],
        sampler->output[j * 4 + 1],
        sampler->output[j * 4 + 2],
        sampler->output[j * 4 + 3],
    };

    if (sampler->interpolation == PB_GLTF_INTERPOLATION_STEP) {
        memcpy(out, a, sizeof(a));
        quat_normalize(out);
        return;
    }

    const float alpha = (time - t0) / fmaxf(t1 - t0, 1e-8f);
    quat_slerp(a, b, alpha, out);
}

static float sampler_duration(const pb_gltf_anim_sampler *sampler)
{
    if (sampler->input_count == 0) {
        return 0.0f;
    }
    return sampler->input[sampler->input_count - 1];
}

bool pb_gltf_scene_load_nodes_and_animations(const cgltf_data *data, pb_gltf_scene *scene)
{
    if (!data || !scene) {
        return false;
    }

    scene->node_count = (uint32_t)data->nodes_count;
    if (scene->node_count == 0) {
        return true;
    }

    scene->nodes = calloc(scene->node_count, sizeof(*scene->nodes));
    if (!scene->nodes) {
        return false;
    }

    for (uint32_t i = 0; i < scene->node_count; ++i) {
        const cgltf_node *src = &data->nodes[i];
        pb_gltf_node *dst = &scene->nodes[i];

        if (src->parent) {
            const cgltf_size parent_index = cgltf_node_index(data, src->parent);
            if (parent_index >= data->nodes_count) {
                return false;
            }
            dst->parent = (uint32_t)parent_index;
        } else {
            dst->parent = PB_GLTF_NO_PARENT;
        }

        copy_node_bind_pose(src, dst);
        reset_node_animation_state(dst);
    }

    scene->animation_count = (uint32_t)data->animations_count;
    if (scene->animation_count == 0) {
        pb_log_info(
            "Loaded glTF animation data: %u nodes, %u clips",
            scene->node_count,
            scene->animation_count);
        return true;
    }

    scene->animations = calloc(scene->animation_count, sizeof(*scene->animations));
    if (!scene->animations) {
        return false;
    }

    for (uint32_t a = 0; a < scene->animation_count; ++a) {
        const cgltf_animation *src_anim = &data->animations[a];
        pb_gltf_animation *dst_anim = &scene->animations[a];

        if (src_anim->name) {
            dst_anim->name = strdup(src_anim->name);
        }

        dst_anim->sampler_count = (uint32_t)src_anim->samplers_count;
        if (dst_anim->sampler_count > 0) {
            dst_anim->samplers = calloc(dst_anim->sampler_count, sizeof(*dst_anim->samplers));
            if (!dst_anim->samplers) {
                return false;
            }
        }

        for (uint32_t s = 0; s < dst_anim->sampler_count; ++s) {
            const cgltf_animation_sampler *src_sampler = &src_anim->samplers[s];
            pb_gltf_anim_sampler *dst_sampler = &dst_anim->samplers[s];

            switch (src_sampler->interpolation) {
            case cgltf_interpolation_type_step:
                dst_sampler->interpolation = PB_GLTF_INTERPOLATION_STEP;
                break;
            case cgltf_interpolation_type_cubic_spline:
                dst_sampler->interpolation = PB_GLTF_INTERPOLATION_CUBIC;
                break;
            default:
                dst_sampler->interpolation = PB_GLTF_INTERPOLATION_LINEAR;
                break;
            }

            const bool is_cubic =
                src_sampler->interpolation == cgltf_interpolation_type_cubic_spline;
            const uint32_t components =
                src_sampler->output && src_sampler->output->type == cgltf_type_vec4 ? 4 : 3;

            if (!copy_accessor_floats(
                    src_sampler->input,
                    &dst_sampler->input,
                    &dst_sampler->input_count,
                    1) ||
                !copy_accessor_floats(
                    src_sampler->output,
                    &dst_sampler->output,
                    &dst_sampler->output_count,
                    components)) {
                return false;
            }

            const uint32_t expected_output_count =
                dst_sampler->input_count * (is_cubic ? 3u : 1u);
            if (dst_sampler->output_count != expected_output_count) {
                return false;
            }

            dst_sampler->output_components = components;
            dst_anim->duration = fmaxf(dst_anim->duration, sampler_duration(dst_sampler));
        }

        dst_anim->channel_count = (uint32_t)src_anim->channels_count;
        if (dst_anim->channel_count > 0) {
            dst_anim->channels = calloc(dst_anim->channel_count, sizeof(*dst_anim->channels));
            if (!dst_anim->channels) {
                return false;
            }
        }

        for (uint32_t c = 0; c < dst_anim->channel_count; ++c) {
            const cgltf_animation_channel *src_channel = &src_anim->channels[c];
            pb_gltf_anim_channel *dst_channel = &dst_anim->channels[c];

            if (!src_channel->target_node || !src_channel->sampler) {
                return false;
            }

            const cgltf_size target_index = cgltf_node_index(data, src_channel->target_node);
            const cgltf_size sampler_index =
                cgltf_animation_sampler_index(src_anim, src_channel->sampler);
            if (target_index >= data->nodes_count || sampler_index >= src_anim->samplers_count) {
                return false;
            }

            dst_channel->target_node = (uint32_t)target_index;
            dst_channel->sampler_index = (uint32_t)sampler_index;

            switch (src_channel->target_path) {
            case cgltf_animation_path_type_translation:
                dst_channel->path = PB_GLTF_ANIM_PATH_TRANSLATION;
                break;
            case cgltf_animation_path_type_rotation:
                dst_channel->path = PB_GLTF_ANIM_PATH_ROTATION;
                break;
            case cgltf_animation_path_type_scale:
                dst_channel->path = PB_GLTF_ANIM_PATH_SCALE;
                break;
            default:
                dst_channel->path = PB_GLTF_ANIM_PATH_WEIGHTS;
                break;
            }
        }
    }

    if (!load_skins(data, scene)) {
        return false;
    }

    pb_log_info(
        "Loaded glTF animation data: %u nodes, %u clips",
        scene->node_count,
        scene->animation_count);
    return true;
}

void pb_gltf_scene_free_nodes_and_animations(pb_gltf_scene *scene)
{
    if (!scene) {
        return;
    }

    for (uint32_t a = 0; a < scene->animation_count; ++a) {
        pb_gltf_animation *anim = &scene->animations[a];
        free(anim->name);
        for (uint32_t s = 0; s < anim->sampler_count; ++s) {
            free(anim->samplers[s].input);
            free(anim->samplers[s].output);
        }
        free(anim->samplers);
        free(anim->channels);
    }
    free(scene->animations);
    scene->animations = NULL;
    scene->animation_count = 0;

    free(scene->nodes);
    scene->nodes = NULL;
    scene->node_count = 0;

    for (uint32_t s = 0; s < scene->skin_count; ++s) {
        free(scene->skins[s].joint_nodes);
        free(scene->skins[s].inverse_bind);
    }
    free(scene->skins);
    scene->skins = NULL;
    scene->skin_count = 0;

    free(scene->skin_palette);
    scene->skin_palette = NULL;
    scene->skin_palette_bytes = 0;
}

bool pb_gltf_scene_init_skin_resources(pb_gltf_scene *scene)
{
    if (!scene || !scene->context) {
        return false;
    }

    const size_t block_bytes = PB_GLTF_SKIN_JOINTS_MAX * sizeof(pb_mat4);
    scene->skin_palette_bytes = scene->draw_count > 0 ? (size_t)scene->draw_count * block_bytes : block_bytes;
    scene->skin_palette = calloc(1, scene->skin_palette_bytes);
    if (!scene->skin_palette) {
        return false;
    }

    pb_rhi_buffer_desc desc = {
        .size = scene->skin_palette_bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .memory_usage = PB_RHI_MEMORY_CPU_TO_GPU,
    };

    if (!pb_rhi_buffer_create(scene->context, &desc, &scene->skin_palette_buffer)) {
        free(scene->skin_palette);
        scene->skin_palette = NULL;
        scene->skin_palette_bytes = 0;
        return false;
    }

    pb_gltf_scene_update_skin_palettes(scene);
    return true;
}

void pb_gltf_scene_update_skin_palettes(pb_gltf_scene *scene)
{
    if (!scene || !scene->skin_palette || !scene->draws) {
        return;
    }

    const size_t block_floats = PB_GLTF_SKIN_JOINTS_MAX * 16;
    for (uint32_t d = 0; d < scene->draw_count; ++d) {
        pb_gltf_draw *draw = &scene->draws[d];
        float *palette_block = scene->skin_palette + (size_t)d * block_floats;

        if (draw->skin_index >= scene->skin_count) {
            continue;
        }

        const pb_gltf_skin *skin = &scene->skins[draw->skin_index];
        alignas(16) float inv_mesh[4][4];
        alignas(16) float joint_world[4][4];
        alignas(16) float joint_mesh[4][4];

        mat4_invert(draw->world, inv_mesh);

        for (uint32_t j = 0; j < skin->joint_count; ++j) {
            const uint32_t joint_node = skin->joint_nodes[j];
            if (joint_node >= scene->node_count) {
                continue;
            }

            memcpy(joint_world, scene->nodes[joint_node].world, sizeof(joint_world));
            pb_mat4_mul(inv_mesh, joint_world, joint_mesh);
            pb_mat4_mul(joint_mesh, skin->inverse_bind[j], joint_mesh);

            memcpy(palette_block + (size_t)j * 16, joint_mesh, sizeof(joint_mesh));
        }
    }

    if (scene->skin_palette_buffer.size > 0) {
        pb_rhi_buffer_upload(
            scene->context,
            &scene->skin_palette_buffer,
            scene->skin_palette,
            scene->skin_palette_bytes);
    }
}

void pb_gltf_scene_sync_node_transforms(pb_gltf_scene *scene)
{
    if (!scene || !scene->nodes) {
        return;
    }

    for (uint32_t i = 0; i < scene->node_count; ++i) {
        pb_gltf_node *node = &scene->nodes[i];
        const bool animated =
            node->anim_translation_active || node->anim_rotation_active || node->anim_scale_active;

        if (!animated && node->has_matrix) {
            memcpy(node->local, node->bind_matrix, sizeof(node->local));
            continue;
        }

        trs_to_mat4(node->anim_translation, node->anim_rotation, node->anim_scale, node->local);
    }

    for (uint32_t pass = 0; pass < scene->node_count; ++pass) {
        bool changed = false;
        for (uint32_t i = 0; i < scene->node_count; ++i) {
            pb_gltf_node *node = &scene->nodes[i];
            alignas(16) float world[4][4];
            alignas(16) float parent_world[4][4];

            if (node->parent == PB_GLTF_NO_PARENT || node->parent >= scene->node_count) {
                memcpy(world, node->local, sizeof(world));
            } else {
                memcpy(parent_world, scene->nodes[node->parent].world, sizeof(parent_world));
                pb_mat4_mul(parent_world, node->local, world);
            }

            if (memcmp(world, node->world, sizeof(world)) != 0) {
                memcpy(node->world, world, sizeof(world));
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }
}

void pb_gltf_scene_sync_draw_worlds(pb_gltf_scene *scene)
{
    if (!scene || !scene->draws) {
        return;
    }

    for (uint32_t i = 0; i < scene->draw_count; ++i) {
        pb_gltf_draw *draw = &scene->draws[i];
        if (draw->node_index < scene->node_count) {
            memcpy(draw->world, scene->nodes[draw->node_index].world, sizeof(draw->world));
        }
    }
}

bool pb_gltf_scene_apply_animation(pb_gltf_scene *scene, uint32_t clip_index, float time_seconds)
{
    if (!scene || !scene->nodes || clip_index >= scene->animation_count) {
        return false;
    }

    const pb_gltf_animation *clip = &scene->animations[clip_index];

    for (uint32_t i = 0; i < scene->node_count; ++i) {
        reset_node_animation_state(&scene->nodes[i]);
    }

    for (uint32_t c = 0; c < clip->channel_count; ++c) {
        const pb_gltf_anim_channel *channel = &clip->channels[c];
        if (channel->target_node >= scene->node_count || channel->sampler_index >= clip->sampler_count) {
            continue;
        }

        const pb_gltf_anim_sampler *sampler = &clip->samplers[channel->sampler_index];
        pb_gltf_node *node = &scene->nodes[channel->target_node];

        switch (channel->path) {
        case PB_GLTF_ANIM_PATH_TRANSLATION:
            sample_vec3(sampler, time_seconds, node->anim_translation);
            node->anim_translation_active = true;
            break;
        case PB_GLTF_ANIM_PATH_ROTATION:
            sample_quat(sampler, time_seconds, node->anim_rotation);
            node->anim_rotation_active = true;
            break;
        case PB_GLTF_ANIM_PATH_SCALE:
            sample_vec3(sampler, time_seconds, node->anim_scale);
            node->anim_scale_active = true;
            break;
        default:
            break;
        }
    }

    pb_gltf_scene_sync_node_transforms(scene);
    pb_gltf_scene_sync_draw_worlds(scene);
    pb_gltf_scene_update_skin_palettes(scene);
    return true;
}
