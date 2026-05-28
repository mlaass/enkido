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
# MSVC's per-literal limit is ~16380 chars (error C2026 "string too
# big"). Long .ak files (e.g. scale_quantize.ak ~94KB) must be emitted
# as multiple adjacent raw-string literals which the C++ compiler
# concatenates at parse time. We split well under the limit (target
# 12000 bytes per chunk) and extend each chunk to the next newline so
# a logical line is never bisected.
set(_chunk_target 12000)

foreach(_ak ${_ak_files})
    get_filename_component(_name "${_ak}" NAME_WE)
    file(READ "${_ak}" _content)
    string(LENGTH "${_content}" _total_len)
    file(APPEND "${OUTPUT_HEADER}"
         "constexpr std::string_view STDLIB_${_name}_SOURCE =\n")

    set(_pos 0)
    while(_pos LESS _total_len)
        math(EXPR _remaining "${_total_len} - ${_pos}")
        if(_remaining LESS_EQUAL ${_chunk_target})
            set(_take ${_remaining})
        else()
            # Take chunk_target bytes, then extend to the next newline
            # so we don't split a source line across two literals.
            math(EXPR _probe_end "${_pos} + ${_chunk_target}")
            math(EXPR _search_len "${_total_len} - ${_probe_end}")
            string(SUBSTRING "${_content}" ${_probe_end} ${_search_len} _tail)
            string(FIND "${_tail}" "\n" _nl_pos)
            if(_nl_pos EQUAL -1)
                set(_take ${_remaining})
            else()
                math(EXPR _take "${_chunk_target} + ${_nl_pos} + 1")
            endif()
        endif()
        string(SUBSTRING "${_content}" ${_pos} ${_take} _chunk)
        file(APPEND "${OUTPUT_HEADER}" "    R\"akstdlib(")
        file(APPEND "${OUTPUT_HEADER}" "${_chunk}")
        file(APPEND "${OUTPUT_HEADER}" ")akstdlib\"\n")
        math(EXPR _pos "${_pos} + ${_take}")
    endwhile()

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
