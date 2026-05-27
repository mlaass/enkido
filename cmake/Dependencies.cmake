# External dependencies for Nkido

include(FetchContent)

# Catch2 for testing
if(NKIDO_BUILD_TESTS)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.2
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    # Include Catch2's CMake helpers for test discovery
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

# rtmidi for live MIDI input. Used by tools/nkido only; the WASM build
# uses the Web MIDI API instead (see docs/prd-midi-input.md §4.10). Gated on
# NOT EMSCRIPTEN so emcmake builds skip the fetch entirely.
if(NOT EMSCRIPTEN)
    set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        rtmidi
        GIT_REPOSITORY https://github.com/thestk/rtmidi.git
        GIT_TAG 6.0.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(rtmidi)
endif()
