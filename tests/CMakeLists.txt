# Copyright 2026 The Peaberry Authors
# SPDX-License-Identifier: Apache-2.0

if(NOT PEABERRY_ASSET_DIR)
    set(_espresso_assets "${CMAKE_SOURCE_DIR}/../espresso/assets")
    if(EXISTS "${_espresso_assets}/models/test_cube.gltf")
        set(PEABERRY_ASSET_DIR "${_espresso_assets}")
    else()
        message(FATAL_ERROR
            "Test assets not found. Clone espresso as a sibling directory "
            "(../espresso) or pass -DPEABERRY_ASSET_DIR=/path/to/espresso/assets")
    endif()
endif()

if(NOT EXISTS "${PEABERRY_ASSET_DIR}/models/test_cube.gltf")
    message(FATAL_ERROR "PEABERRY_ASSET_DIR is missing test_cube.gltf: ${PEABERRY_ASSET_DIR}")
endif()

message(STATUS "PEABERRY_ASSET_DIR=${PEABERRY_ASSET_DIR}")

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
    test_frustum_cull.c
    test_ring_buffer.c
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
    PEABERRY_ASSET_DIR="${PEABERRY_ASSET_DIR}"
)

add_dependencies(peaberry_tests peaberry_shaders)

add_test(NAME peaberry_tests COMMAND peaberry_tests)

set_tests_properties(peaberry_tests PROPERTIES
    LABELS "unit;gpu"
)
