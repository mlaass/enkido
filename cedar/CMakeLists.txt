# Cedar - Audio Synthesis Engine
# Can be built standalone: cmake -B build /path/to/cedar

cmake_minimum_required(VERSION 3.21)

# Allow standalone build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(cedar
        VERSION 0.1.0
        DESCRIPTION "Cedar audio synthesis engine"
        LANGUAGES CXX
    )

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    option(CEDAR_BUILD_TESTS "Build Cedar tests" ON)

    # Standalone: create minimal compiler options
    add_library(enkido_compiler_options INTERFACE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(enkido_compiler_options INTERFACE -Wall -Wextra -Wpedantic)
    endif()
else()
    set(CEDAR_BUILD_TESTS ${ENKIDO_BUILD_TESTS})
endif()

# Cedar library
add_library(cedar STATIC
    src/cedar.cpp
    src/vm/vm.cpp
)

target_include_directories(cedar
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Apply compiler options (use BUILD_INTERFACE to avoid export issues)
target_link_libraries(cedar
    PRIVATE
        $<BUILD_INTERFACE:enkido_compiler_options>
)

# Alias for use in parent project
add_library(enkido::cedar ALIAS cedar)

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
