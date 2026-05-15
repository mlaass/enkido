# Cedar - Audio Synthesis Engine
# Can be built standalone: cmake -B build /path/to/cedar

cmake_minimum_required(VERSION 3.21)

# Allow standalone build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # Read version from parent VERSION file
    set(_version_file "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
    if(EXISTS "${_version_file}")
        file(STRINGS "${_version_file}" _cedar_version)
    else()
        set(_cedar_version "0.0.0")
    endif()
    project(cedar
        VERSION ${_cedar_version}
        DESCRIPTION "Cedar audio synthesis engine"
        LANGUAGES CXX
    )

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    option(CEDAR_BUILD_TESTS "Build Cedar tests" ON)

    # Standalone: create minimal compiler options
    add_library(nkido_compiler_options INTERFACE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(nkido_compiler_options INTERFACE -Wall -Wextra -Wpedantic)
    endif()
else()
    set(CEDAR_BUILD_TESTS ${NKIDO_BUILD_TESTS})
endif()

# Feature toggles (all ON by default for full builds)
option(CEDAR_ENABLE_AUDIO_DECODERS "Include MP3/FLAC/OGG decoders" ON)
option(CEDAR_ENABLE_SOUNDFONT      "Include SoundFont (SF2) support" ON)
option(CEDAR_ENABLE_FFT            "Include FFT support (KissFFT)" ON)
option(CEDAR_ENABLE_FILE_IO        "Include filesystem-based file loading" ON)
option(CEDAR_ENABLE_MINBLEP        "Include MinBLEP anti-aliased oscillators" ON)
option(CEDAR_FLOAT_ONLY            "Use float instead of double for beat timing" OFF)

# Memory constant overrides (empty = use defaults)
set(CEDAR_BLOCK_SIZE "" CACHE STRING "Override BLOCK_SIZE (default 128)")
set(CEDAR_MAX_BUFFERS "" CACHE STRING "Override MAX_BUFFERS (default 256)")
set(CEDAR_MAX_STATES "" CACHE STRING "Override MAX_STATES (default 512)")
set(CEDAR_MAX_VARS "" CACHE STRING "Override MAX_VARS (default 4096)")
set(CEDAR_MAX_PROGRAM_SIZE "" CACHE STRING "Override MAX_PROGRAM_SIZE (default 4096)")
set(CEDAR_MAX_ENV_PARAMS "" CACHE STRING "Override MAX_ENV_PARAMS (default 256)")
set(CEDAR_ARENA_SIZE "" CACHE STRING "Override AudioArena size in bytes (default 33554432)")

# Core sources (always compiled)
set(CEDAR_SOURCES
    src/cedar.cpp
    src/vm/vm.cpp
    src/opcodes/midi.cpp
    src/io/smf_parser.cpp
    src/io/uri_resolver.cpp
    src/io/handlers/bundled_handler.cpp
    src/io/handlers/github_handler.cpp
)

# Conditional sources
if(CEDAR_ENABLE_AUDIO_DECODERS)
    list(APPEND CEDAR_SOURCES src/io/audio_decoder.cpp)
endif()

if(CEDAR_ENABLE_SOUNDFONT)
    list(APPEND CEDAR_SOURCES src/audio/soundfont.cpp)
endif()

if(CEDAR_ENABLE_FFT)
    list(APPEND CEDAR_SOURCES src/dsp/fft.cpp)
    # Wavetable preprocessor + registry depend on kissfft for offline FFT
    list(APPEND CEDAR_SOURCES
        src/wavetable/preprocessor.cpp
        src/wavetable/registry.cpp
    )
endif()

if(CEDAR_ENABLE_MINBLEP)
    list(APPEND CEDAR_SOURCES
        src/opcodes/minblep_table.cpp
        src/opcodes/minblep_state.cpp
    )
endif()

if(CEDAR_ENABLE_FILE_IO AND NOT EMSCRIPTEN)
    list(APPEND CEDAR_SOURCES
        src/io/file_loader.cpp
        src/io/file_cache.cpp
        src/io/handlers/file_handler.cpp
        src/io/handlers/http_handler.cpp
    )
endif()

# Cedar library
add_library(cedar STATIC ${CEDAR_SOURCES})

target_include_directories(cedar
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
# Vendored headers as SYSTEM so their warnings don't get flagged by -Wall.
target_include_directories(cedar SYSTEM PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>
)

# Apply compiler options (use BUILD_INTERFACE to avoid export issues)
target_link_libraries(cedar
    PRIVATE
        $<BUILD_INTERFACE:nkido_compiler_options>
)

# Native HTTP support: link OpenSSL for HTTPS via cpp-httplib (header-only).
# Skipped on Emscripten — TS handles HTTP on the web side.
if(CEDAR_ENABLE_FILE_IO AND NOT EMSCRIPTEN)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(cedar PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# Inject version as compile definitions (read from VERSION file)
target_compile_definitions(cedar PUBLIC
    CEDAR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    CEDAR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    CEDAR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# Feature toggle compile definitions
if(NOT CEDAR_ENABLE_AUDIO_DECODERS)
    target_compile_definitions(cedar PUBLIC CEDAR_NO_AUDIO_DECODERS=1)
endif()
if(NOT CEDAR_ENABLE_SOUNDFONT)
    target_compile_definitions(cedar PUBLIC CEDAR_NO_SOUNDFONT=1)
endif()
if(NOT CEDAR_ENABLE_FFT)
    target_compile_definitions(cedar PUBLIC CEDAR_NO_FFT=1)
endif()
if(NOT CEDAR_ENABLE_FILE_IO OR EMSCRIPTEN)
    target_compile_definitions(cedar PUBLIC CEDAR_NO_FILE_IO=1)
endif()
if(NOT CEDAR_ENABLE_MINBLEP)
    target_compile_definitions(cedar PUBLIC CEDAR_NO_MINBLEP=1)
endif()
if(CEDAR_FLOAT_ONLY)
    target_compile_definitions(cedar PUBLIC CEDAR_FLOAT_ONLY=1)
endif()

# Memory constant overrides
foreach(_var IN ITEMS CEDAR_BLOCK_SIZE CEDAR_MAX_BUFFERS CEDAR_MAX_STATES
                      CEDAR_MAX_VARS CEDAR_MAX_PROGRAM_SIZE CEDAR_MAX_ENV_PARAMS
                      CEDAR_ARENA_SIZE)
    if(NOT "${${_var}}" STREQUAL "")
        target_compile_definitions(cedar PUBLIC ${_var}=${${_var}})
    endif()
endforeach()

# Enable position-independent code for linking into shared libraries (Python bindings)
set_target_properties(cedar PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Add Python bindings (not available for Emscripten/WASM builds)
if(NOT EMSCRIPTEN)
    add_subdirectory(bindings)
endif()

# Alias for use in parent project
add_library(nkido::cedar ALIAS cedar)

# Tests
if(CEDAR_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Installation
include(GNUInstallDirs)

install(TARGETS cedar
    EXPORT cedarTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/cedar
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT cedarTargets
    FILE cedarTargets.cmake
    NAMESPACE cedar::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cedar
)
