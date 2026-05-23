# cmake/generate_stdlib_files.cmake
#
# Reads every `${STDLIB_SRC_DIR}/*.ak` and writes a single embed header at
# `${OUTPUT_HEADER}` exposing each file as a constexpr std::string_view plus a
# `STDLIB_EMBEDDED_FILES` table. Invoked via `cmake -P` from akkado's
# add_custom_command, so any edit to a .ak file triggers regeneration on the
# next build.
#
# Required cache vars on entry: STDLIB_SRC_DIR, OUTPUT_HEADER.

if(NOT DEFINED STDLIB_SRC_DIR OR NOT DEFINED OUTPUT_HEADER)
    message(FATAL_ERROR "generate_stdlib_files.cmake requires STDLIB_SRC_DIR and OUTPUT_HEADER")
endif()

file(GLOB _ak_files "${STDLIB_SRC_DIR}/*.ak")
list(SORT _ak_files)

get_filename_component(_out_dir "${OUTPUT_HEADER}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")

# Write static header preamble. Bracket arguments [==[...]==] preserve
# semicolons that CMake would otherwise treat as list separators.
file(WRITE "${OUTPUT_HEADER}"
[==[// AUTO-GENERATED — DO NOT EDIT.
//
// Generated from akkado/stdlib/*.ak by cmake/generate_stdlib_files.cmake.
// To add a new stdlib file: drop `name.ak` into akkado/stdlib/. The next build
// regenerates this header automatically (add_custom_command DEPENDS).

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
foreach(_ak ${_ak_files})
    get_filename_component(_name "${_ak}" NAME_WE)
    file(READ "${_ak}" _content)
    file(APPEND "${OUTPUT_HEADER}"
         "constexpr std::string_view STDLIB_${_name}_SOURCE = R\"akstdlib(\n")
    file(APPEND "${OUTPUT_HEADER}" "${_content}")
    file(APPEND "${OUTPUT_HEADER}" ")akstdlib\"")
    file(APPEND "${OUTPUT_HEADER}" [==[;

]==])
    math(EXPR _count "${_count} + 1")
endforeach()

file(APPEND "${OUTPUT_HEADER}"
     "constexpr std::array<StdlibFile, ${_count}> STDLIB_EMBEDDED_FILES = {{\n")
foreach(_ak ${_ak_files})
    get_filename_component(_name "${_ak}" NAME_WE)
    file(APPEND "${OUTPUT_HEADER}"
         "    StdlibFile{\"<stdlib/${_name}.ak>\", STDLIB_${_name}_SOURCE},\n")
endforeach()
file(APPEND "${OUTPUT_HEADER}"
[==[}};

}  // namespace akkado
]==])
