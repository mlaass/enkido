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

# Option to enable concurrency tests (requires thread support)
option(CEDAR_ENABLE_CONCURRENCY_TESTS "Enable thread safety tests" ON)

add_executable(cedar_tests
    test_cedar.cpp
    test_vm.cpp
    test_audio_arena.cpp
    test_buffer_pool.cpp
    test_state_pool.cpp
    test_env_map.cpp
    test_crossfade.cpp
    test_sample_bank.cpp
    test_memory_stress.cpp
    test_pattern_query.cpp
)

# Add concurrency tests if enabled
if(CEDAR_ENABLE_CONCURRENCY_TESTS)
    target_sources(cedar_tests PRIVATE test_concurrency.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(cedar_tests PRIVATE Threads::Threads)
endif()

target_link_libraries(cedar_tests
    PRIVATE
        cedar
        Catch2::Catch2WithMain
)

catch_discover_tests(cedar_tests)
