# Cedar tests

# Standalone build: need to fetch Catch2
if(NOT TARGET Catch2::Catch2WithMain)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

include(Catch)

add_executable(cedar_tests
    test_cedar.cpp
    test_vm.cpp
)

target_link_libraries(cedar_tests
    PRIVATE
        cedar
        Catch2::Catch2WithMain
)

catch_discover_tests(cedar_tests)
