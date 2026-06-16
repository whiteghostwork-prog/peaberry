# Copyright 2026 The Peaberry Authors
# SPDX-License-Identifier: Apache-2.0

add_executable(peaberry_tests
    main.c
    test_runner.c
    test_math.c
    test_vulkan.c
    test_gltf.c
    test_gltf_render.c
    test_bench.c
    test_draw_sort.c
    test_shadow.c
)

target_include_directories(peaberry_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(peaberry_tests
    PRIVATE
        peaberry
        m
)

target_compile_definitions(peaberry_tests PRIVATE
    PEABERRY_SHADER_DIR="${CMAKE_BINARY_DIR}/shaders"
    PEABERRY_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
)

add_dependencies(peaberry_tests peaberry_shaders)

add_test(NAME peaberry_tests COMMAND peaberry_tests)

set_tests_properties(peaberry_tests PROPERTIES
    LABELS "unit;gpu"
)
