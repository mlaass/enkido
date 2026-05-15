# Nkido WASM Build
# Build with: emcmake cmake -B build && cmake --build build

cmake_minimum_required(VERSION 3.21)

# Read version so cedar's compile definitions resolve correctly
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" NKIDO_VERSION)

project(nkido_wasm
    VERSION ${NKIDO_VERSION}
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Disable thread-safe statics for WASM - AudioWorklet doesn't support pthreads
# properly and __cxa_guard_acquire aborts when trying to acquire mutex
if(EMSCRIPTEN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-threadsafe-statics")
endif()

# Find parent project libraries
# When building standalone, these paths need to be set
set(NKIDO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# Include compiler options (defines nkido_compiler_options interface library)
if(NOT TARGET nkido_compiler_options)
    include("${NKIDO_ROOT}/cmake/CompilerOptions.cmake")
endif()

if(NOT TARGET cedar)
    add_subdirectory("${NKIDO_ROOT}/cedar" "${CMAKE_BINARY_DIR}/cedar")
endif()

if(NOT TARGET akkado)
    add_subdirectory("${NKIDO_ROOT}/akkado" "${CMAKE_BINARY_DIR}/akkado")
endif()

# WASM module
add_executable(nkido_wasm nkido_wasm.cpp)

target_link_libraries(nkido_wasm
    PRIVATE
        cedar
        akkado
)

if(EMSCRIPTEN)
    # Emscripten-specific settings
    set_target_properties(nkido_wasm PROPERTIES
        OUTPUT_NAME "nkido"
        SUFFIX ".js"
    )

    # Build exported functions list
    set(NKIDO_EXPORTED_FUNCTIONS
        # Cedar VM
        _cedar_init
        _cedar_destroy
        _cedar_set_sample_rate
        _cedar_set_bpm
        _cedar_set_crossfade_blocks
        _cedar_load_program
        _cedar_process_block
        _cedar_get_output_left
        _cedar_get_output_right
        # Audio input (audio-input PRD §4.5)
        _cedar_get_input_left
        _cedar_get_input_right
        _cedar_set_input_active
        _cedar_set_input_source
        _cedar_get_input_source
        # MIDI input (prd-midi-input §4.10)
        _cedar_push_midi_event
        _cedar_set_default_midi_device
        _cedar_get_default_midi_device
        _cedar_apply_midi_sources
        _cedar_reset
        _cedar_is_crossfading
        _cedar_crossfade_position
        _cedar_has_program
        _cedar_set_param
        _cedar_set_param_slew
        # Sample management
        _cedar_load_sample
        _cedar_load_audio_data
        _cedar_has_sample
        _cedar_get_sample_id
        _cedar_clear_samples
        _cedar_get_sample_count
        # Wavetable management (Smooch)
        _cedar_load_wavetable_wav
        _cedar_clear_wavetables
        _cedar_has_wavetable
        _cedar_wavetable_find_id
        _cedar_wavetable_count
        # State initialization
        _cedar_apply_state_inits
        # Akkado compiler
        _akkado_compile
        _akkado_get_bytecode
        _akkado_get_bytecode_size
        _akkado_get_diagnostic_count
        _akkado_get_diagnostic_severity
        _akkado_get_diagnostic_message
        _akkado_get_diagnostic_line
        _akkado_get_diagnostic_column
        _akkado_clear_result
        # Required samples
        _akkado_get_required_samples_count
        _akkado_get_required_sample
        _akkado_resolve_sample_ids
        _akkado_patch_sample_ids_in_bytecode
        # Required SoundFonts
        _akkado_get_required_soundfonts_count
        _akkado_get_required_soundfont_filename
        _akkado_get_required_soundfont_preset
        # Required wavetables (Smooch)
        _akkado_get_required_wavetables_count
        _akkado_get_required_wavetable_name
        _akkado_get_required_wavetable_path
        # Required URIs (samples() and friends)
        _akkado_get_required_uri_count
        _akkado_get_required_uri
        _akkado_get_required_uri_kind
        # Required input sources (audio-input PRD §4.4)
        _akkado_get_required_input_sources_count
        _akkado_get_required_input_source
        # Required MIDI sources (prd-midi-input §4.7)
        _akkado_get_required_midi_sources_count
        _akkado_get_required_midi_source_state_id
        _akkado_get_required_midi_source_kind
        _akkado_get_required_midi_source_name
        _akkado_get_required_midi_source_channel
        _akkado_get_required_midi_source_loop
        _akkado_get_required_midi_source_tempo
        # State init accessors
        _akkado_get_state_init_count
        _akkado_get_state_init_id
        _akkado_get_state_init_type
        # Visualization declarations
        _akkado_get_viz_count
        _akkado_get_viz_name
        _akkado_get_viz_type
        _akkado_get_viz_state_id
        _akkado_get_viz_options
        _akkado_get_viz_source_offset
        _akkado_get_viz_source_length
        _akkado_get_viz_pattern_index
        # Editor metadata APIs
        _akkado_get_builtins_json
        _akkado_get_shape_index
        # Probe data API
        _cedar_get_probe_sample_count
        _cedar_get_probe_data
        _cedar_get_probe_write_pos
        # FFT Probe data API
        _cedar_get_fft_bin_count
        _cedar_get_fft_magnitudes
        _cedar_get_fft_frame_counter
        # SoundFont API
        _cedar_load_soundfont
        _cedar_soundfont_preset_count
        _cedar_soundfont_presets_json
        _cedar_soundfont_count
        # Utility
        _nkido_get_block_size
        _nkido_malloc
        _nkido_free
    )
    list(JOIN NKIDO_EXPORTED_FUNCTIONS "," NKIDO_EXPORTED_FUNCTIONS_STR)

    target_link_options(nkido_wasm PRIVATE
        # Export C functions
        "-sEXPORTED_FUNCTIONS=[${NKIDO_EXPORTED_FUNCTIONS_STR}]"

        # Export runtime functions for memory access
        "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,getValue,setValue,UTF8ToString,stringToUTF8,lengthBytesUTF8,wasmMemory"

        # Memory settings
        -sINITIAL_MEMORY=268435456   # 256MB initial (SoundFont parsing needs ~190MB peak)
        -sALLOW_MEMORY_GROWTH=1
        -sMAXIMUM_MEMORY=1073741824  # 1GB max (SF3 SoundFonts expand ~6x during decode)

        # Stack size - default 64KB is too small for deep AST recursion in codegen
        -sSTACK_SIZE=1048576    # 1MB

        # No filesystem needed
        -sNO_FILESYSTEM=1

        # Module settings
        -sMODULARIZE=1
        "-sEXPORT_NAME=createNkidoModule"
        -sENVIRONMENT=web,worker,shell

        # Note: Not using Emscripten's built-in AudioWorklet support (-sAUDIO_WORKLET=1)
        # because we manually load WASM in cedar-processor.js. Those flags enable
        # pthreads which conflict with AudioWorklet's limited threading model.

        # Single file output (inline WASM as base64)
        # Uncomment for single-file distribution:
        # -sSINGLE_FILE=1
    )

    # Build-type specific options
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_link_options(nkido_wasm PRIVATE
            -O1
            -g
            -gsource-map

            -sASSERTIONS=0
        )
    else()
        target_link_options(nkido_wasm PRIVATE
            -O3
            -flto
        )
    endif()

    # Copy output to static directory
    add_custom_command(TARGET nkido_wasm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/../static/wasm"
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE_DIR:nkido_wasm>/nkido.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/../static/wasm/"
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE_DIR:nkido_wasm>/nkido.wasm"
            "${CMAKE_CURRENT_SOURCE_DIR}/../static/wasm/"
        COMMENT "Copying WASM files to static/wasm/"
    )

    # Copy source map in debug builds for browser debugging
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(TARGET nkido_wasm POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE_DIR:nkido_wasm>/nkido.wasm.map"
                "${CMAKE_CURRENT_SOURCE_DIR}/../static/wasm/"
            COMMENT "Copying WASM source map to static/wasm/"
        )
    endif()
endif()
