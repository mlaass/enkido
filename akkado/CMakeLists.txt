# Akkado - Audio DSL Compiler
# Depends on Cedar for bytecode definitions

cmake_minimum_required(VERSION 3.21)

# Allow standalone build (requires cedar to be installed or provided)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # Read version from parent VERSION file
    set(_version_file "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
    if(EXISTS "${_version_file}")
        file(STRINGS "${_version_file}" _akkado_version)
    else()
        set(_akkado_version "0.0.0")
    endif()
    project(akkado
        VERSION ${_akkado_version}
        DESCRIPTION "Akkado audio DSL compiler"
        LANGUAGES CXX
    )

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    option(AKKADO_BUILD_TESTS "Build Akkado tests" ON)

    # Standalone: create minimal compiler options
    add_library(nkido_compiler_options INTERFACE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(nkido_compiler_options INTERFACE -Wall -Wextra -Wpedantic)
    endif()

    # Find Cedar (must be installed or provided via CMAKE_PREFIX_PATH)
    find_package(cedar QUIET)
    if(NOT cedar_FOUND)
        message(STATUS "Cedar not found, building from ../cedar")
        add_subdirectory(../cedar cedar_build)
    endif()
else()
    set(AKKADO_BUILD_TESTS ${NKIDO_BUILD_TESTS})
endif()

# --------------------------------------------------------------------------
# Stdlib file embedding — see prd-runtime-event-transforms.md §9 Phase 2b.
# Generates `akkado/include/akkado/generated/stdlib_files.hpp` from every
# `akkado/stdlib/*.ak`. Built as an add_custom_command output so editing any
# .ak file triggers regeneration on the next build without re-running cmake.
# --------------------------------------------------------------------------
set(_akkado_stdlib_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/stdlib")
set(_akkado_stdlib_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/akkado/generated")
set(_akkado_stdlib_gen_header "${_akkado_stdlib_gen_dir}/stdlib_files.hpp")
set(_akkado_stdlib_gen_script
    "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/generate_stdlib_files.cmake")

file(GLOB _akkado_stdlib_files CONFIGURE_DEPENDS "${_akkado_stdlib_src_dir}/*.ak")
list(SORT _akkado_stdlib_files)

add_custom_command(
    OUTPUT  ${_akkado_stdlib_gen_header}
    COMMAND ${CMAKE_COMMAND}
            -DSTDLIB_SRC_DIR=${_akkado_stdlib_src_dir}
            -DOUTPUT_HEADER=${_akkado_stdlib_gen_header}
            -P ${_akkado_stdlib_gen_script}
    DEPENDS ${_akkado_stdlib_files} ${_akkado_stdlib_gen_script}
    COMMENT "Generating akkado/generated/stdlib_files.hpp from akkado/stdlib/*.ak"
    VERBATIM
)

add_custom_target(akkado_stdlib_embed
    DEPENDS ${_akkado_stdlib_gen_header}
)

# Akkado library
add_library(akkado STATIC
    src/akkado.cpp
    src/compile_context.cpp
    src/diagnostics.cpp
    src/lexer.cpp
    src/parser.cpp
    src/mini_lexer.cpp
    src/mini_parser.cpp
    src/pattern_eval.cpp
    src/symbol_table.cpp
    src/analyzer.cpp
    src/const_eval.cpp
    src/codegen.cpp
    src/codegen_arrays.cpp
    src/codegen_functions.cpp
    src/codegen_control_flow.cpp
    src/codegen_higher_order.cpp
    src/codegen_state.cpp
    src/codegen_params.cpp
    src/codegen_patterns.cpp
    src/codegen_stereo.cpp
    src/codegen_viz.cpp
    src/codegen/options.cpp
    src/builtins_json.cpp
    src/shape_index.cpp
    src/typed_value.cpp
    src/ast_hash.cpp
    src/chord_parser.cpp
    src/voicing.cpp
    src/pattern_debug.cpp
    src/tuning.cpp
    src/source_map.cpp
    src/file_resolver.cpp
    src/import_scanner.cpp
)

target_include_directories(akkado
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:include>
)

# Apply compiler options (use BUILD_INTERFACE to avoid export issues)
target_link_libraries(akkado
    PUBLIC
        cedar
    PRIVATE
        $<BUILD_INTERFACE:nkido_compiler_options>
)

# Inject version as compile definitions (read from VERSION file)
target_compile_definitions(akkado PUBLIC
    AKKADO_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    AKKADO_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    AKKADO_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

add_dependencies(akkado akkado_stdlib_embed)

# Alias for use in parent project
add_library(nkido::akkado ALIAS akkado)

# Tests
if(AKKADO_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Installation
include(GNUInstallDirs)

install(TARGETS akkado
    EXPORT akkadoTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/akkado
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT akkadoTargets
    FILE akkadoTargets.cmake
    NAMESPACE akkado::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/akkado
)
