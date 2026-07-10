# Akkado tests

# Standalone build: need to fetch Catch2
if(CMAKE_SOURCE_DIR STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/..")
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

include(Catch)

add_executable(akkado_tests
    akkado_test_main.cpp
    test_akkado.cpp
    test_analyzer.cpp
    test_builtins.cpp
    test_builtins_json.cpp
    test_shape_index.cpp
    test_codegen.cpp
    test_codegen_cycle_timing.cpp
    test_pattern_playback_e2e.cpp
    test_control_flow.cpp
    test_fn_annotations.cpp
    test_block_call.cpp
    test_higher_order.cpp
    test_types.cpp
    test_const_eval.cpp
    test_diagnostics.cpp
    test_fm_detection.cpp
    test_lexer.cpp
    test_parser.cpp
    test_mini_notation.cpp
    test_ast_arena.cpp
    test_buffer_allocator.cpp
    test_pattern_debug.cpp
    test_pattern_highlight_offsets.cpp
    test_pattern_event.cpp
    test_sample_registry.cpp
    test_symbol_table.cpp
    test_chord.cpp
    test_arrays.cpp
    test_source_map.cpp
    test_file_resolver.cpp
    test_import_scanner.cpp
    test_import.cpp
    test_methods.cpp
    test_state.cpp
    test_bus_routing.cpp
    test_pattern_event_arrays.cpp
    test_event_map.cpp
    test_scale_quantize.cpp
    test_fast_slow.cpp
    test_reorder.cpp
    test_param_type_annotations.cpp
    test_overload.cpp
    test_pattern_scalar.cpp
    test_transport.cpp
    test_sample_emission_golden.cpp
    test_hot_swap_determinism.cpp
    test_hot_swap_record_options.cpp
    test_fuzz_determinism.cpp
    test_fuzz_recompile_audio.cpp
    test_hot_swap_event_transforms.cpp
    test_state_init_buffer_codec.cpp
    test_drift_fuzz.cpp
)

# Host extension tests only exist when the API is compiled in.
if(CEDAR_HOST_EXTENSIONS)
    target_sources(akkado_tests PRIVATE test_host_extensions.cpp)
endif()

target_link_libraries(akkado_tests
    PRIVATE
        akkado
        Catch2::Catch2  # custom main lives in akkado_test_main.cpp
)

# Leg 4 of docs/prd-memory-integrity-tests.md: the drift fuzz seeds from the
# stdlib + a curated corpus. Embed both directories as compile-time strings so
# the test doesn't depend on the runtime working directory.
target_compile_definitions(akkado_tests PRIVATE
    AKKADO_STDLIB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../stdlib"
    AKKADO_TEST_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus"
)

# Phase 0 of prd-parser-codegen-correctness.md: bytecode-disassembly
# snapshot harness. Reuses the formatter from tools/nkido, so the
# test only builds when tools are enabled (default ON).
if(NKIDO_BUILD_TOOLS)
    target_sources(akkado_tests PRIVATE test_bytecode_snapshot.cpp)
    target_link_libraries(akkado_tests PRIVATE nkido_bytecode_dump)
    target_compile_definitions(akkado_tests
        PRIVATE
            AKKADO_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
            AKKADO_TEST_SNAPSHOTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/snapshots"
    )
endif()

# BUILD_SHARED_LIBS is ON in this tree (vcpkg toolchain), so Catch2 is
# built as a DLL. The Catch2WithMain target inlines main() and pulls the
# Catch2 implementation static-ish; the plain Catch2 target we link
# against (custom main lives in akkado_test_main.cpp) leaves Catch2d.dll
# as a runtime dependency that has to sit next to the .exe — both for
# catch_discover_tests' build-time enumeration and for ctest runs.
# $<TARGET_RUNTIME_DLLS> only sees IMPORTED SHARED targets, so the
# in-build Catch2 target needs to be copied explicitly via TARGET_FILE.
# (vcpkg's applocal.ps1 step on the link line handles libssl / libcrypto.)
if(WIN32)
    add_custom_command(TARGET akkado_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Catch2::Catch2>
                $<TARGET_FILE_DIR:akkado_tests>
    )
endif()

catch_discover_tests(akkado_tests)
