# cedar/bindings/CMakeLists.txt

# Auto-detect experiments venv Python if available
set(EXPERIMENTS_VENV "${CMAKE_CURRENT_SOURCE_DIR}/../../experiments/.venv")
if(EXISTS "${EXPERIMENTS_VENV}/bin/python3" AND NOT PYTHON_EXECUTABLE)
    set(PYTHON_EXECUTABLE "${EXPERIMENTS_VENV}/bin/python3" CACHE FILEPATH "Python executable")
    message(STATUS "Using experiments venv Python: ${PYTHON_EXECUTABLE}")
endif()

# Try system pybind11 first, then fetch if not found
find_package(pybind11 QUIET)

if(NOT pybind11_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG v2.13.6
    )
    FetchContent_MakeAvailable(pybind11)
endif()

# Python module
pybind11_add_module(cedar_core bindings.cpp)

# Link against the cedar library we defined in the root CMakeLists.txt
target_link_libraries(cedar_core PRIVATE cedar)

# Ensure include directories are correct
target_include_directories(cedar_core PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Output directory for the .so/.pyd file (puts it in experiments folder for easy import)
# Use CMAKE_CURRENT_SOURCE_DIR to get correct path regardless of build root
set_target_properties(cedar_core PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../experiments"
)