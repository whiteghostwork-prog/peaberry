# Copyright 2026 The Peaberry Authors
# SPDX-License-Identifier: Apache-2.0

include(FetchContent)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE)
set(GLFW_BUILD_X11 OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

add_library(peaberry_example_wsi STATIC
    common/wsi.c
)

target_include_directories(peaberry_example_wsi
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/common
)

target_link_libraries(peaberry_example_wsi
    PUBLIC
        peaberry
        glfw
)

add_executable(peaberry_hello_vk hello_vk/main.c)
target_link_libraries(peaberry_hello_vk PRIVATE peaberry_example_wsi m)

set_target_properties(peaberry_hello_vk PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
)

add_executable(peaberry_triangle triangle/main.c)
target_link_libraries(peaberry_triangle PRIVATE peaberry_example_wsi m)
target_compile_definitions(peaberry_triangle PRIVATE
    PEABERRY_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders"
)
add_dependencies(peaberry_triangle peaberry_shaders)

set_target_properties(peaberry_triangle PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
)

add_executable(peaberry_quad quad/main.c)
target_link_libraries(peaberry_quad PRIVATE peaberry_example_wsi m)
target_compile_definitions(peaberry_quad PRIVATE
    PEABERRY_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders"
    PEABERRY_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
)
add_dependencies(peaberry_quad peaberry_shaders)

set_target_properties(peaberry_quad PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
)

add_executable(peaberry_sphere sphere/main.c)
target_link_libraries(peaberry_sphere PRIVATE peaberry_example_wsi m)
target_compile_definitions(peaberry_sphere PRIVATE
    PEABERRY_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders"
    PEABERRY_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
)
add_dependencies(peaberry_sphere peaberry_shaders)

set_target_properties(peaberry_sphere PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples
)
