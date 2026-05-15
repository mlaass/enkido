# Integration tests for nkido-cli: drive the built binary in render mode and
# assert on the produced WAV. Render shares the program-loading code path
# with play/serve/ui (see program_loader.cpp), so a render-mode regression
# transitively guards all three.

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

# Generate a local sample-bank manifest with absolute paths to checked-in
# WAVs in web/static/samples. Lets the sample-pattern test run fully offline.
set(NKIDO_REPO_ROOT "${CMAKE_SOURCE_DIR}")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/local_bank.strudel.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/fixtures/local_bank.strudel.json
    @ONLY
)

# Minimal default-kit manifest used as a fake NKIDO_DEFAULT_KIT in tests so
# they don't depend on the full 38-entry kit being discoverable.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/default_kit_minimal.strudel.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/fixtures/default_kit_minimal.strudel.json
    @ONLY
)

# User-supplied bank that maps `bd` to a different file than the default
# kit. Used to verify user banks shadow the default kit.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/user_override_bank.strudel.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/fixtures/user_override_bank.strudel.json
    @ONLY
)

# Generate the test_paths header so the test binary knows where to find the
# CLI executable, fixture sources, and the configured manifest.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/test_paths.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/test_paths.hpp
    @ONLY
)

add_executable(nkido_cli_tests
    test_render.cpp
    test_serve.cpp
    test_midi_cc_dispatch.cpp
)

# Make sure the CLI binary is built before tests try to invoke it.
add_dependencies(nkido_cli_tests nkido-cli)

target_include_directories(nkido_cli_tests
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..  # for midi_input.hpp
)

target_link_libraries(nkido_cli_tests
    PRIVATE
        Catch2::Catch2WithMain
        nkido_compiler_options
        cedar  # dispatch_midi_cc inlines into the test; cedar provides VM + EnvMap
)

catch_discover_tests(nkido_cli_tests)
