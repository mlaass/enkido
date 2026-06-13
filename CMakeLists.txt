cmake_minimum_required(VERSION 3.21)

# Read version from single source of truth
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NKIDO_VERSION)

project(nkido
    VERSION ${NKIDO_VERSION}
    DESCRIPTION "High-performance audio synthesis system"
    LANGUAGES CXX
)

# Require C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate compile_commands.json for clangd/LSP
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options
option(NKIDO_BUILD_CEDAR "Build Cedar synth engine" ON)
option(NKIDO_BUILD_AKKADO "Build Akkado compiler" ON)
option(NKIDO_BUILD_TOOLS "Build CLI tools" ON)
option(NKIDO_BUILD_TESTS "Build tests" ON)
# nkido CLI tests (test_serve.cpp) used to be POSIX-only; the Phase 3
# Windows port (tools/nkido/tests/test_subprocess.hpp) made them
# portable. The flag stays as a downstream opt-out (e.g. for the
# godot-nkido-addon CI that doesn't care about CLI integration tests)
# and defaults to the umbrella flag everywhere.
option(NKIDO_BUILD_NKIDO_TESTS "Build nkido CLI tests" ${NKIDO_BUILD_TESTS})

# Include CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Coverage)
include(Dependencies)

# Add components
if(NKIDO_BUILD_CEDAR)
    add_subdirectory(cedar)
endif()

if(NKIDO_BUILD_AKKADO)
    if(NOT NKIDO_BUILD_CEDAR)
        message(FATAL_ERROR "Akkado requires Cedar. Set NKIDO_BUILD_CEDAR=ON")
    endif()
    add_subdirectory(akkado)
endif()

if(NKIDO_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# Testing
if(NKIDO_BUILD_TESTS)
    enable_testing()
endif()

# Windows-only: stage the repo LICENSE into the install tree with the
# conventional .txt extension so the future release zip carries it
# without leaking a renamed file into Linux/macOS install layouts.
if(WIN32)
    include(GNUInstallDirs)
    install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/nkido
        RENAME LICENSE.txt
    )
endif()
