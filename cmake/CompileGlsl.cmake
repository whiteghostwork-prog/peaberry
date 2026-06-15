find_program(PEABERRY_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/bin")
find_program(PEABERRY_GLSLANG_VALIDATOR glslangValidator)

if(NOT PEABERRY_GLSLC AND NOT PEABERRY_GLSLANG_VALIDATOR)
    message(FATAL_ERROR "Neither glslc nor glslangValidator found. Install glslang-tools.")
endif()

function(peaberry_compile_glsl OUTPUT_SPIRV_FILES)
    set(options)
    set(oneValueArgs TARGET)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "peaberry_compile_glsl requires TARGET")
    endif()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "peaberry_compile_glsl requires SOURCES")
    endif()

    set(generated_spirv "")
    foreach(source ${ARG_SOURCES})
        get_filename_component(source_name "${source}" NAME)
        set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${source_name}.spv")
        if(PEABERRY_GLSLC)
            set(compile_cmd "${PEABERRY_GLSLC}" "${source}" -o "${output}")
        else()
            set(compile_cmd "${PEABERRY_GLSLANG_VALIDATOR}" -V "${source}" -o "${output}")
        endif()
        add_custom_command(
            OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
            COMMAND ${compile_cmd}
            DEPENDS "${source}"
            COMMENT "Compiling ${source_name} to SPIR-V"
            VERBATIM
        )
        list(APPEND generated_spirv "${output}")
    endforeach()

    add_custom_target(${ARG_TARGET} DEPENDS ${generated_spirv})
    set(${OUTPUT_SPIRV_FILES} ${generated_spirv} PARENT_SCOPE)
endfunction()
