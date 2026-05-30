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

#include "peaberry/peaberry.h"

#include "wsi.h"

#include <math.h>
#include <stdio.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

int main(void)
{
    pb_context_desc ctx_desc = {
        .app_name = "peaberry hello_vk",
        .enable_validation = true,
        .enable_surface = true,
    };

    pb_context *ctx = pb_context_create(&ctx_desc);
    if (!ctx) {
        fprintf(stderr, "Failed to create peaberry context\n");
        return 1;
    }

    pb_example_wsi_desc wsi_desc = {
        .context = ctx,
        .width = 1280,
        .height = 720,
        .title = "peaberry hello_vk",
    };

    pb_example_wsi *wsi = pb_example_wsi_create(&wsi_desc);
    if (!wsi) {
        fprintf(stderr, "Failed to create example window\n");
        pb_context_destroy(ctx);
        return 1;
    }

    GLFWwindow *window = pb_example_wsi_window(wsi);
    double start = glfwGetTime();

    while (!pb_example_wsi_should_close(wsi)) {
        pb_example_wsi_poll(wsi);

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        double t = glfwGetTime() - start;
        float r = 0.5f + 0.5f * sinf((float)t * 0.7f);
        float g = 0.5f + 0.5f * sinf((float)t * 1.1f + 2.0f);
        float b = 0.5f + 0.5f * sinf((float)t * 0.9f + 4.0f);

        if (pb_example_wsi_begin_frame(wsi, r, g, b, 1.0f)) {
            pb_example_wsi_end_frame(wsi);
        }
    }

    pb_example_wsi_destroy(wsi);
    pb_context_destroy(ctx);
    return 0;
}
