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
    test_akkado.cpp
    test_analyzer.cpp
    test_builtins.cpp
    test_builtins_json.cpp
    test_shape_index.cpp
    test_codegen.cpp
    test_codegen_cycle_timing.cpp
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
    test_pattern_scalar.cpp
    test_transport.cpp
    test_sample_emission_golden.cpp
    test_hot_swap_determinism.cpp
    test_fuzz_determinism.cpp
    test_fuzz_recompile_audio.cpp
)

target_link_libraries(akkado_tests
    PRIVATE
        akkado
        Catch2::Catch2WithMain
)

catch_discover_tests(akkado_tests)
