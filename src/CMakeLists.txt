# Copyright 2026 The Peaberry Authors
# SPDX-License-Identifier: Apache-2.0

add_library(peaberry STATIC
    core/log.c
    core/math.c
    pb_context.c
    vk/context.c
    rhi/alloc.c
    rhi/buffer.c
    rhi/ring_buffer.c
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
    pbr/draw_sort.c
    pbr/exposure_pass.c
    pbr/bloom_pass.c
    pbr/frustum_cull.c
    pbr/pbr_forward_pass.c
    pbr/point_shadow_pass.c
    pbr/post_pass.c
    pbr/shadow_pass.c
    pbr/sphere_pass.c
    load/gltf.c
    load/gltf_animation.c
    load/tangent.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/mikktspace/mikktspace.c
)

add_dependencies(peaberry peaberry_shaders)

target_include_directories(peaberry
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/mikktspace
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

include(GNUInstallDirs)

install(
    TARGETS peaberry volk
    EXPORT PeaberryTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../include/peaberry
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    EXPORT PeaberryTargets
    FILE PeaberryTargets.cmake
    NAMESPACE Peaberry::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Peaberry
)
