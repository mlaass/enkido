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
# Reads `akkado/stdlib/*.ak` and emits a generated header that exposes each
# file as a constexpr std::string_view + a STDLIB_EMBEDDED_FILES table.
# Re-runs at configure time; CONFIGURE_DEPENDS makes CMake re-glob on change.
# --------------------------------------------------------------------------
set(_akkado_stdlib_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/stdlib")
set(_akkado_stdlib_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/akkado/generated")
set(_akkado_stdlib_gen_header "${_akkado_stdlib_gen_dir}/stdlib_files.hpp")
file(MAKE_DIRECTORY ${_akkado_stdlib_gen_dir})

file(GLOB _akkado_stdlib_files CONFIGURE_DEPENDS "${_akkado_stdlib_src_dir}/*.ak")
list(SORT _akkado_stdlib_files)

# Write the header in pieces using file(WRITE)/(APPEND) so CMake's list-separator
# handling doesn't eat the semicolons in the generated C++ source.
file(WRITE ${_akkado_stdlib_gen_header}
[==[// AUTO-GENERATED — DO NOT EDIT.
//
// Generated from akkado/stdlib/*.ak by akkado/CMakeLists.txt at configure time.
// To add a new stdlib file: drop `name.ak` into akkado/stdlib/ and re-run cmake.

#pragma once

#include <array>
#include <string_view>

namespace akkado {

struct StdlibFile {
    std::string_view name;
    std::string_view source;
};

]==])

set(_count 0)
foreach(_ak ${_akkado_stdlib_files})
    get_filename_component(_name ${_ak} NAME_WE)
    file(READ ${_ak} _content)
    file(APPEND ${_akkado_stdlib_gen_header}
         "constexpr std::string_view STDLIB_${_name}_SOURCE = R\"akstdlib(\n")
    file(APPEND ${_akkado_stdlib_gen_header} "${_content}")
    file(APPEND ${_akkado_stdlib_gen_header} ")akstdlib\"")
    file(APPEND ${_akkado_stdlib_gen_header} [==[;

]==])
    math(EXPR _count "${_count} + 1")
endforeach()

file(APPEND ${_akkado_stdlib_gen_header}
     "constexpr std::array<StdlibFile, ${_count}> STDLIB_EMBEDDED_FILES = {{\n")
foreach(_ak ${_akkado_stdlib_files})
    get_filename_component(_name ${_ak} NAME_WE)
    file(APPEND ${_akkado_stdlib_gen_header}
         "    StdlibFile{\"<stdlib/${_name}.ak>\", STDLIB_${_name}_SOURCE},\n")
endforeach()
file(APPEND ${_akkado_stdlib_gen_header}
[==[}};

}  // namespace akkado
]==])
unset(_count)

# Akkado library
add_library(akkado STATIC
    src/akkado.cpp
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
