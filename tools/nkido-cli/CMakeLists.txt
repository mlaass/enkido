# Nkido CLI - Unified synth tool with SDL2 audio output

find_package(SDL2 REQUIRED)

add_executable(nkido-cli
    main.cpp
    audio_engine.cpp
    asset_loader.cpp
    bytecode_loader.cpp
    bytecode_dump.cpp
    midi_input.cpp
    program_loader.cpp
    serve_mode.cpp
    ui/bitmap_font.cpp
    ui/text_buffer.cpp
    ui/ui_mode.cpp
)

target_link_libraries(nkido-cli
    PRIVATE
        akkado
        cedar
        SDL2::SDL2
        rtmidi
        nkido_compiler_options
)

target_include_directories(nkido-cli
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

# Built-in default sample kit. The compile-time path makes in-tree builds
# resolve `s"bd"` etc. without setup; installed builds fall back to the
# `share/nkido/default_kit/strudel.json` rule below.
target_compile_definitions(nkido-cli PRIVATE
    NKIDO_DEFAULT_KIT_PATH="${CMAKE_SOURCE_DIR}/web/static/samples/bpb_808_clean/strudel.json"
    NKIDO_SOUNDFONT_PATH="${CMAKE_SOURCE_DIR}/web/static/soundfonts"
)

# Platform-specific: SDL2main needed on Windows
if(WIN32)
    target_link_libraries(nkido-cli PRIVATE SDL2::SDL2main)
endif()

install(TARGETS nkido-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/web/static/samples/bpb_808_clean/
    DESTINATION share/nkido/default_kit
    FILES_MATCHING
        PATTERN "*.wav"
        PATTERN "strudel.json"
)

# Bundled SoundFonts (matches the web app's DEFAULT_SOUNDFONTS table). The
# `resolve_soundfont_alias()` helper in asset_loader.cpp searches this
# directory at runtime so `soundfont(@, "gm", 0)` works without --soundfont.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/web/static/soundfonts/
    DESTINATION share/nkido/soundfonts
    FILES_MATCHING
        PATTERN "*.sf2"
        PATTERN "*.sf3"
        PATTERN "LICENSE.md"
)

if(NKIDO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
