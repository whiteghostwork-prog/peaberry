/*
 * Copyright 2026 The Peaberry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pbr/draw_sort.h"

#include <string.h>

float pb_draw_sort_view_depth(const pb_mat4 view, const pb_mat4 model)
{
    pb_mat4 view_copy;
    pb_mat4 model_copy;
    pb_mat4 view_model;

    memcpy(view_copy, view, sizeof(view_copy));
    memcpy(model_copy, model, sizeof(model_copy));
    pb_mat4_mul(view_copy, model_copy, view_model);
    return view_model[3][2];
}

void pb_draw_sort_stable(
    pb_draw_sort_entry *entries,
    uint32_t count,
    bool back_to_front)
{
    if (!entries || count < 2) {
        return;
    }

    for (uint32_t i = 1; i < count; ++i) {
        const pb_draw_sort_entry key = entries[i];
        int32_t j = (int32_t)i - 1;

        while (j >= 0) {
            const pb_draw_sort_entry *cur = &entries[(uint32_t)j];
            bool move = false;

            if (back_to_front) {
                move = cur->view_depth < key.view_depth ||
                    (cur->view_depth == key.view_depth && cur->draw_index > key.draw_index);
            } else {
                move = cur->view_depth > key.view_depth ||
                    (cur->view_depth == key.view_depth && cur->draw_index > key.draw_index);
            }

            if (!move) {
                break;
            }

            entries[(uint32_t)j + 1] = entries[(uint32_t)j];
            --j;
        }

        entries[(uint32_t)j + 1] = key;
    }
}
