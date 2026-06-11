# Copyright 2026 The Peaberry Authors
# SPDX-License-Identifier: Apache-2.0

add_library(peaberry STATIC
    core/log.c
    core/math.c
    pb_context.c
    vk/context.c
    rhi/alloc.c
    rhi/buffer.c
    rhi/cmd_submit.c
    rhi/cubemap.c
    rhi/mesh.c
    rhi/offscreen.c
    rhi/query_pool.c
    rhi/frame_metrics.c
    rhi/quad.c
    rhi/shader.c
    rhi/texture.c
    rhi/triangle.c
    pbr/ibl.c
    pbr/pbr_forward_pass.c
    pbr/sphere_pass.c
    load/gltf.c
    load/tangent.c
)

add_dependencies(peaberry peaberry_shaders)

target_include_directories(peaberry
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${stb_SOURCE_DIR}
        ${cgltf_SOURCE_DIR}
)

target_compile_definitions(peaberry
    PRIVATE
        $<$<BOOL:${PEABERRY_ENABLE_VALIDATION}>:PEABERRY_ENABLE_VALIDATION=1>
        _POSIX_C_SOURCE=200809L
)

target_link_libraries(peaberry
    PUBLIC
        volk
        Vulkan::Vulkan
    PRIVATE
        cglm
)
