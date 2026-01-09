#pragma once

#include "bitmap_font.hpp"
#include "text_buffer.hpp"
#include "../audio_engine.hpp"
#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <cstdint>

namespace enkido::ui {

class UIMode {
public:
    UIMode() = default;
    ~UIMode();

    UIMode(const UIMode&) = delete;
    UIMode& operator=(const UIMode&) = delete;

    bool init(std::uint32_t sample_rate, std::uint32_t buffer_size);
    int run();  // Main loop, returns exit code

private:
    // Event handling
    void handle_event(const SDL_Event& event);
    void handle_key(const SDL_KeyboardEvent& key);
    void handle_text_input(const SDL_TextInputEvent& text);

    // Rendering
    void render();
    void render_gutter();
    void render_editor();
    void render_status_bar();
    void render_cursor();
    void render_error_highlights();

    // Actions
    void compile_and_play();
    void stop_playback();

    // SDL
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    BitmapFont font_;

    // Components
    TextBuffer buffer_;
    AudioEngine engine_;

    // State
    bool should_quit_ = false;
    bool playing_ = false;
    std::string status_message_;
    std::vector<std::size_t> error_lines_;
    std::uint32_t cursor_blink_time_ = 0;
    bool cursor_visible_ = true;

    // Scroll state
    std::size_t scroll_y_ = 0;

    // Window dimensions
    int window_width_ = 800;
    int window_height_ = 600;

    // Layout constants
    static constexpr int GUTTER_WIDTH = 48;
    static constexpr int STATUS_HEIGHT = 28;
    static constexpr int PADDING = 8;

    // Colors
    static constexpr SDL_Color BG_COLOR       = {30, 30, 30, 255};
    static constexpr SDL_Color GUTTER_BG      = {40, 40, 40, 255};
    static constexpr SDL_Color TEXT_COLOR     = {220, 220, 220, 255};
    static constexpr SDL_Color LINE_NUM_COLOR = {100, 100, 100, 255};
    static constexpr SDL_Color CURSOR_COLOR   = {255, 255, 255, 255};
    static constexpr SDL_Color ERROR_BG       = {80, 40, 40, 255};
    static constexpr SDL_Color STATUS_BG      = {50, 50, 50, 255};
    static constexpr SDL_Color STATUS_OK      = {80, 200, 120, 255};
    static constexpr SDL_Color STATUS_ERR     = {255, 100, 100, 255};
    static constexpr SDL_Color STATUS_PLAY    = {100, 180, 255, 255};
};

}  // namespace enkido::ui
