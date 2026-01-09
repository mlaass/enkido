# Enkido WASM Build
# Build with: emcmake cmake -B build && cmake --build build

cmake_minimum_required(VERSION 3.21)
project(enkido_wasm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find parent project libraries
# When building standalone, these paths need to be set
set(ENKIDO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# Include compiler options (defines enkido_compiler_options interface library)
if(NOT TARGET enkido_compiler_options)
    include("${ENKIDO_ROOT}/cmake/CompilerOptions.cmake")
endif()

if(NOT TARGET cedar)
    add_subdirectory("${ENKIDO_ROOT}/cedar" "${CMAKE_BINARY_DIR}/cedar")
endif()

if(NOT TARGET akkado)
    add_subdirectory("${ENKIDO_ROOT}/akkado" "${CMAKE_BINARY_DIR}/akkado")
endif()

# WASM module
add_executable(enkido_wasm enkido_wasm.cpp)

target_link_libraries(enkido_wasm
    PRIVATE
        cedar
        akkado
)

if(EMSCRIPTEN)
    # Emscripten-specific settings
    set_target_properties(enkido_wasm PROPERTIES
        OUTPUT_NAME "enkido"
        SUFFIX ".js"
    )

    # Build exported functions list
    set(ENKIDO_EXPORTED_FUNCTIONS
        _cedar_init
        _cedar_destroy
        _cedar_set_sample_rate
        _cedar_set_bpm
        _cedar_set_crossfade_blocks
        _cedar_load_program
        _cedar_process_block
        _cedar_get_output_left
        _cedar_get_output_right
        _cedar_reset
        _cedar_is_crossfading
        _cedar_crossfade_position
        _cedar_has_program
        _cedar_set_param
        _cedar_set_param_slew
        _akkado_compile
        _akkado_get_bytecode
        _akkado_get_bytecode_size
        _akkado_get_diagnostic_count
        _akkado_get_diagnostic_severity
        _akkado_get_diagnostic_message
        _akkado_get_diagnostic_line
        _akkado_get_diagnostic_column
        _akkado_clear_result
        _enkido_get_block_size
        _enkido_malloc
        _enkido_free
    )
    list(JOIN ENKIDO_EXPORTED_FUNCTIONS "," ENKIDO_EXPORTED_FUNCTIONS_STR)

    target_link_options(enkido_wasm PRIVATE
        # Export C functions
        "-sEXPORTED_FUNCTIONS=[${ENKIDO_EXPORTED_FUNCTIONS_STR}]"

        # Export runtime functions for memory access
        "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,getValue,setValue,UTF8ToString,stringToUTF8,lengthBytesUTF8"

        # Memory settings
        -sINITIAL_MEMORY=33554432  # 32MB initial
        -sALLOW_MEMORY_GROWTH=1
        -sMAXIMUM_MEMORY=134217728  # 128MB max

        # Optimization
        -O3
        -flto

        # No filesystem needed
        -sNO_FILESYSTEM=1

        # Module settings
        -sMODULARIZE=1
        "-sEXPORT_NAME=createEnkidoModule"
        "-sENVIRONMENT=web,worker"

        # For AudioWorklet support
        -sAUDIO_WORKLET=1
        -sWASM_WORKERS=1

        # Single file output (inline WASM as base64)
        # Uncomment for single-file distribution:
        # -sSINGLE_FILE=1
    )

    # Copy output to static directory
    add_custom_command(TARGET enkido_wasm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/../static/wasm"
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE_DIR:enkido_wasm>/enkido.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/../static/wasm/"
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE_DIR:enkido_wasm>/enkido.wasm"
            "${CMAKE_CURRENT_SOURCE_DIR}/../static/wasm/"
        COMMENT "Copying WASM files to static/wasm/"
    )
endif()
