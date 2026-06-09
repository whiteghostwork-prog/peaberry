# Copyright 2026 The Peaberry Authors
# SPDX-License-Identifier: Apache-2.0

add_executable(peaberry_bench
    bench_main.c
    bench_stats.c
    bench_target.c
    scenario_clear.c
)

target_include_directories(peaberry_bench
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(peaberry_bench
    PRIVATE
        peaberry
        m
)

set_target_properties(peaberry_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/benchmarks
)
