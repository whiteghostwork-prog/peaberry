# Copyright 2026 The Peaberry Authors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.25)

project(peaberry VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PEABERRY_BUILD_EXAMPLES "Build peaberry examples (deprecated: use espresso repo)" OFF)
option(PEABERRY_BUILD_TESTS "Build and register peaberry tests" ON)
option(PEABERRY_BUILD_BENCHMARKS "Build peaberry_bench GPU benchmark runner (deprecated: use espresso repo)" OFF)
option(PEABERRY_ENABLE_VALIDATION "Enable Vulkan validation layers" ON)

find_package(Vulkan REQUIRED)

include(FetchContent)

FetchContent_Declare(
    volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        1.4.304
)
FetchContent_MakeAvailable(volk)

FetchContent_Declare(
    cglm
    GIT_REPOSITORY https://github.com/recp/cglm.git
    GIT_TAG        v0.9.4
)
FetchContent_MakeAvailable(cglm)

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        31c1ad37456438565541f4919958214b6e762fb4
)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

FetchContent_Declare(
    cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.14
)
FetchContent_GetProperties(cgltf)
if(NOT cgltf_POPULATED)
    FetchContent_Populate(cgltf)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileGlsl.cmake)

peaberry_compile_glsl(PEABERRY_SHADER_SPIRV
    TARGET peaberry_shaders
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.frag
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/quad.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/quad.frag
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pbr_forward.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pbr_forward.frag
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/fullscreen.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/brdf_lut.frag
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/equirect_to_cubemap.frag
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/irradiance.frag
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/prefilter.frag
)

add_subdirectory(src)

if(PEABERRY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(PEABERRY_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(PEABERRY_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/shaders/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/peaberry/shaders
    FILES_MATCHING PATTERN "*.vert" PATTERN "*.frag"
)

install(
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileGlsl.cmake
    DESTINATION ${CMAKE_INSTALL_DATADIR}/peaberry/cmake
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PeaberryConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/PeaberryConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Peaberry
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/PeaberryConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/PeaberryConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/PeaberryConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Peaberry
)
