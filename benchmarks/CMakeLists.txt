# Copyright 2026 The Peaberry Authors
# SPDX-License-Identifier: Apache-2.0

add_executable(peaberry_bench
    bench_main.c
    bench_baseline.c
    bench_stats.c
    bench_target.c
    scenario_clear.c
    scenario_sphere.c
    scenario_gltf.c
)

target_include_directories(peaberry_bench
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(peaberry_bench PRIVATE
    PEABERRY_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders"
    PEABERRY_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
)

target_link_libraries(peaberry_bench
    PRIVATE
        peaberry
        m
)

add_dependencies(peaberry_bench peaberry_shaders)

set_target_properties(peaberry_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/benchmarks
)
