#ifndef PEABERRY_RHI_SHADER_H
#define PEABERRY_RHI_SHADER_H

#include <volk.h>

#include <stdbool.h>
#include <stddef.h>

bool pb_rhi_shader_module_from_file(
    VkDevice device,
    const char *path,
    VkShaderModule *out_module);

#endif
