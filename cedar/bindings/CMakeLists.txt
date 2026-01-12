# cedar/bindings/CMakeLists.txt
find_package(pybind11 REQUIRED)

# Python module
pybind11_add_module(cedar_core bindings.cpp)

# Link against the cedar library we defined in the root CMakeLists.txt
target_link_libraries(cedar_core PRIVATE cedar)

# Ensure include directories are correct
target_include_directories(cedar_core PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Output directory for the .so/.pyd file (puts it in experiments folder for easy import)
set_target_properties(cedar_core PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../experiments"
)