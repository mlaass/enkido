# Cedar tests

# Standalone build: need to fetch Catch2
if(NOT TARGET Catch2::Catch2WithMain)
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

# Option to enable concurrency tests (requires thread support)
option(CEDAR_ENABLE_CONCURRENCY_TESTS "Enable thread safety tests" ON)

add_executable(cedar_tests
    test_cedar.cpp
    test_vm.cpp
    test_arrays.cpp
    test_audio_arena.cpp
    test_buffer_pool.cpp
    test_state_pool.cpp
    test_env_map.cpp
    test_crossfade.cpp
    test_hot_swap.cpp
    test_sample_bank.cpp
    test_memory_stress.cpp
    test_sequence.cpp
    test_poly.cpp
    test_seqpat_values.cpp
    test_midi.cpp
    test_file_loader.cpp
    test_uri_resolver.cpp
    test_audio_decoder.cpp
    test_fft.cpp
    test_wavetable.cpp
    test_edge_op.cpp
    test_state_op.cpp
    test_phaser.cpp
    test_drywet.cpp
    test_hot_swap_extended_params.cpp
    test_seqpat_gate.cpp
    test_seqpat_step.cpp
    test_event_map.cpp
    test_event_rate_scale.cpp
    test_event_reorder.cpp
    test_event_fanout.cpp
    test_soundfont_registry.cpp
    test_audit_inst_rate.cpp
    test_subprogram_table.cpp
)

# Add concurrency tests if enabled
if(CEDAR_ENABLE_CONCURRENCY_TESTS)
    target_sources(cedar_tests PRIVATE test_concurrency.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(cedar_tests PRIVATE Threads::Threads)
endif()

target_link_libraries(cedar_tests
    PRIVATE
        cedar
        Catch2::Catch2WithMain
)

# Source-tree path for test fixtures (e.g. .mid files used by smf parser
# tests). Embedded as a compile-time string so tests don't depend on the
# current working directory at runtime.
target_compile_definitions(cedar_tests PRIVATE
    CEDAR_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    CEDAR_OPCODES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../include/cedar/opcodes"
)

# On Windows with BUILD_SHARED_LIBS=ON (vcpkg toolchain), Catch2 is built
# as a DLL. Catch2WithMain depends on Catch2, so both DLLs have to sit
# next to the exe for catch_discover_tests and ctest. vcpkg's
# applocal.ps1 only handles imported targets (libssl/libcrypto), so copy
# Catch2's DLLs explicitly.
if(WIN32)
    add_custom_command(TARGET cedar_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Catch2::Catch2>
                $<TARGET_FILE:Catch2::Catch2WithMain>
                $<TARGET_FILE_DIR:cedar_tests>
    )
endif()

catch_discover_tests(cedar_tests)
