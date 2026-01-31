cmake_minimum_required(VERSION 3.21)

project(enkido
    VERSION 0.1.0
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
option(ENKIDO_BUILD_CEDAR "Build Cedar synth engine" ON)
option(ENKIDO_BUILD_AKKADO "Build Akkado compiler" ON)
option(ENKIDO_BUILD_TOOLS "Build CLI tools" ON)
option(ENKIDO_BUILD_TESTS "Build tests" ON)

# Include CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Coverage)
include(Dependencies)

# Add components
if(ENKIDO_BUILD_CEDAR)
    add_subdirectory(cedar)
endif()

if(ENKIDO_BUILD_AKKADO)
    if(NOT ENKIDO_BUILD_CEDAR)
        message(FATAL_ERROR "Akkado requires Cedar. Set ENKIDO_BUILD_CEDAR=ON")
    endif()
    add_subdirectory(akkado)
endif()

if(ENKIDO_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# Testing
if(ENKIDO_BUILD_TESTS)
    enable_testing()
endif()
