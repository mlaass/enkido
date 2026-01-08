# Akkado - Audio DSL Compiler
# Depends on Cedar for bytecode definitions

cmake_minimum_required(VERSION 3.21)

# Allow standalone build (requires cedar to be installed or provided)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(akkado
        VERSION 0.1.0
        DESCRIPTION "Akkado audio DSL compiler"
        LANGUAGES CXX
    )

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    option(AKKADO_BUILD_TESTS "Build Akkado tests" ON)

    # Standalone: create minimal compiler options
    add_library(enkido_compiler_options INTERFACE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(enkido_compiler_options INTERFACE -Wall -Wextra -Wpedantic)
    endif()

    # Find Cedar (must be installed or provided via CMAKE_PREFIX_PATH)
    find_package(cedar QUIET)
    if(NOT cedar_FOUND)
        message(STATUS "Cedar not found, building from ../cedar")
        add_subdirectory(../cedar cedar_build)
    endif()
else()
    set(AKKADO_BUILD_TESTS ${ENKIDO_BUILD_TESTS})
endif()

# Akkado library
add_library(akkado STATIC
    src/akkado.cpp
    src/diagnostics.cpp
)

target_include_directories(akkado
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Apply compiler options (use BUILD_INTERFACE to avoid export issues)
target_link_libraries(akkado
    PUBLIC
        cedar
    PRIVATE
        $<BUILD_INTERFACE:enkido_compiler_options>
)

# Alias for use in parent project
add_library(enkido::akkado ALIAS akkado)

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
