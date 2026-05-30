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

#ifndef PEABERRY_H
#define PEABERRY_H

#include <stdbool.h>
#include <stdint.h>

#define PEABERRY_VERSION_MAJOR 0
#define PEABERRY_VERSION_MINOR 1
#define PEABERRY_VERSION_PATCH 0

typedef struct pb_context pb_context;

typedef struct pb_context_desc {
    const char *app_name;
    bool enable_validation;
    /* Enable WSI instance extensions (surface + Wayland). Required for swapchain apps. */
    bool enable_surface;
} pb_context_desc;

pb_context *pb_context_create(const pb_context_desc *desc);
void pb_context_destroy(pb_context *ctx);

#endif
