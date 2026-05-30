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

#include "rhi/shader.h"

#include "core/log.h"

#include <stdio.h>
#include <stdlib.h>

bool pb_rhi_shader_module_from_file(
    VkDevice device,
    const char *path,
    VkShaderModule *out_module)
{
    if (!path || !out_module) {
        return false;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        pb_log_error("Failed to open shader SPIR-V: %s", path);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }

    long size = ftell(file);
    if (size <= 0) {
        fclose(file);
        pb_log_error("Invalid shader SPIR-V size: %s", path);
        return false;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    uint32_t *code = malloc((size_t)size);
    if (!code) {
        fclose(file);
        return false;
    }

    if (fread(code, 1, (size_t)size, file) != (size_t)size) {
        free(code);
        fclose(file);
        return false;
    }
    fclose(file);

    if (size % 4 != 0) {
        free(code);
        pb_log_error("SPIR-V file size is not a multiple of 4: %s", path);
        return false;
    }

    VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)size,
        .pCode = code,
    };

    VkResult result = vkCreateShaderModule(device, &create_info, NULL, out_module);
    free(code);

    if (result != VK_SUCCESS) {
        pb_log_error("vkCreateShaderModule failed for %s", path);
        return false;
    }

    return true;
}
