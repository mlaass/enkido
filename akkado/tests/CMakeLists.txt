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
    test_codegen.cpp
    test_diagnostics.cpp
    test_fm_detection.cpp
    test_lexer.cpp
    test_parser.cpp
    test_mini_notation.cpp
    test_ast_arena.cpp
    test_buffer_allocator.cpp
    test_symbol_table.cpp
    test_chord.cpp
)

target_link_libraries(akkado_tests
    PRIVATE
        akkado
        Catch2::Catch2WithMain
)

catch_discover_tests(akkado_tests)
