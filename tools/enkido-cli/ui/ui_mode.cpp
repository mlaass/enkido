#include "ui_mode.hpp"
#include "../bytecode_loader.hpp"
#include "akkado/akkado.hpp"
#include <cstring>
#include <sstream>

namespace enkido::ui {

UIMode::~UIMode() {
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
    }
    if (window_) {
        SDL_DestroyWindow(window_);
    }
    SDL_StopTextInput();
    SDL_Quit();
}

bool UIMode::init(std::uint32_t sample_rate, std::uint32_t buffer_size) {
    // Initialize SDL with video and audio
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        return false;
    }

    // Create window
    window_ = SDL_CreateWindow(
        "Enkido",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        window_width_, window_height_,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (!window_) {
        return false;
    }

    // Create renderer
    renderer_ = SDL_CreateRenderer(window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        return false;
    }

    // Initialize font
    if (!font_.init(renderer_)) {
        return false;
    }

    // Initialize audio engine
    AudioEngine::Config audio_config{sample_rate, buffer_size, 2};
    if (!engine_.init(audio_config)) {
        return false;
    }

    // Enable text input
    SDL_StartTextInput();

    // Set initial status
    status_message_ = "Ready | Shift+Enter: Play | Esc: Stop";

    return true;
}

int UIMode::run() {
    std::uint32_t last_time = SDL_GetTicks();

    while (!should_quit_) {
        // Handle events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            handle_event(event);
        }

        // Update cursor blink
        std::uint32_t now = SDL_GetTicks();
        cursor_blink_time_ += now - last_time;
        last_time = now;
        if (cursor_blink_time_ >= 500) {
            cursor_visible_ = !cursor_visible_;
            cursor_blink_time_ = 0;
        }

        // Render
        render();
        SDL_RenderPresent(renderer_);

        // Small delay to reduce CPU usage (vsync should handle most of it)
        SDL_Delay(1);
    }

    engine_.stop();
    return 0;
}

void UIMode::handle_event(const SDL_Event& event) {
    switch (event.type) {
        case SDL_QUIT:
            should_quit_ = true;
            break;

        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                window_width_ = event.window.data1;
                window_height_ = event.window.data2;
            }
            break;

        case SDL_KEYDOWN:
            handle_key(event.key);
            break;

        case SDL_TEXTINPUT:
            handle_text_input(event.text);
            break;

        default:
            break;
    }
}

void UIMode::handle_key(const SDL_KeyboardEvent& key) {
    // Reset cursor blink on any key
    cursor_visible_ = true;
    cursor_blink_time_ = 0;

    bool ctrl = (key.keysym.mod & KMOD_CTRL) != 0;
    bool shift = (key.keysym.mod & KMOD_SHIFT) != 0;

    switch (key.keysym.sym) {
        case SDLK_ESCAPE:
            if (playing_) {
                stop_playback();
            }
            break;

        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (shift) {
                compile_and_play();
            } else {
                buffer_.newline();
                error_lines_.clear();
            }
            break;

        case SDLK_BACKSPACE:
            buffer_.backspace();
            error_lines_.clear();
            break;

        case SDLK_DELETE:
            buffer_.delete_char();
            error_lines_.clear();
            break;

        case SDLK_LEFT:
            buffer_.move_left();
            break;

        case SDLK_RIGHT:
            buffer_.move_right();
            break;

        case SDLK_UP:
            buffer_.move_up();
            break;

        case SDLK_DOWN:
            buffer_.move_down();
            break;

        case SDLK_HOME:
            if (ctrl) {
                buffer_.move_to_start();
            } else {
                buffer_.home();
            }
            break;

        case SDLK_END:
            if (ctrl) {
                buffer_.move_to_end();
            } else {
                buffer_.end();
            }
            break;

        case SDLK_a:
            if (ctrl) {
                buffer_.select_all();
            }
            break;

        case SDLK_v:
            if (ctrl) {
                char* clipboard = SDL_GetClipboardText();
                if (clipboard) {
                    buffer_.insert_text(clipboard);
                    SDL_free(clipboard);
                    error_lines_.clear();
                }
            }
            break;

        case SDLK_c:
            if (ctrl && buffer_.has_selection()) {
                std::string selection(buffer_.get_selection());
                SDL_SetClipboardText(selection.c_str());
            }
            break;

        case SDLK_x:
            if (ctrl && buffer_.has_selection()) {
                std::string selection(buffer_.get_selection());
                SDL_SetClipboardText(selection.c_str());
                buffer_.delete_selection();
                error_lines_.clear();
            }
            break;

        default:
            break;
    }

    // Update scroll to keep cursor visible
    std::size_t cursor_line = buffer_.cursor_line();
    int visible_lines = (window_height_ - STATUS_HEIGHT - PADDING * 2) / font_.line_height();
    if (cursor_line < scroll_y_) {
        scroll_y_ = cursor_line;
    } else if (cursor_line >= scroll_y_ + static_cast<std::size_t>(visible_lines)) {
        scroll_y_ = cursor_line - static_cast<std::size_t>(visible_lines) + 1;
    }
}

void UIMode::handle_text_input(const SDL_TextInputEvent& text) {
    // Filter control characters (handled by handle_key)
    if (text.text[0] >= 32) {
        buffer_.insert_text(text.text);
        error_lines_.clear();

        // Reset cursor blink
        cursor_visible_ = true;
        cursor_blink_time_ = 0;
    }
}

void UIMode::compile_and_play() {
    std::string source(buffer_.text());
    if (source.empty()) {
        status_message_ = "Nothing to compile";
        return;
    }

    // Compile
    auto result = akkado::compile(source, "<editor>");

    if (!result.success) {
        // Collect error lines
        error_lines_.clear();
        std::ostringstream err;
        for (const auto& diag : result.diagnostics) {
            if (diag.location.line > 0) {
                error_lines_.push_back(diag.location.line - 1);  // 0-indexed
            }
            if (err.tellp() > 0) err << "; ";
            err << diag.message;
        }
        status_message_ = "Error: " + err.str();
        playing_ = false;
        return;
    }

    // Convert bytecode to instructions
    std::size_t num_instructions = result.bytecode.size() / sizeof(cedar::Instruction);
    std::vector<cedar::Instruction> instructions(num_instructions);
    std::memcpy(instructions.data(), result.bytecode.data(), result.bytecode.size());

    // Load into VM - use immediate load if not playing, hot-swap if playing
    if (playing_) {
        // Hot-swap for glitch-free transition while playing
        auto load_result = engine_.vm().load_program(instructions);
        if (load_result != cedar::VM::LoadResult::Success) {
            status_message_ = "Error: Failed to load program";
            return;
        }
    } else {
        // Immediate load when stopped (resets VM, avoids slot exhaustion)
        if (!engine_.vm().load_program_immediate(instructions)) {
            status_message_ = "Error: Failed to load program";
            return;
        }
    }

    // Start audio if not already playing
    if (!playing_) {
        if (!engine_.start()) {
            status_message_ = "Error: Failed to start audio";
            return;
        }
        playing_ = true;
    }

    error_lines_.clear();
    std::ostringstream ss;
    ss << "Playing (" << num_instructions << " instructions)";
    status_message_ = ss.str();
}

void UIMode::stop_playback() {
    engine_.pause();  // Use pause so we can restart without re-init
    playing_ = false;
    status_message_ = "Stopped | Shift+Enter: Play";
}

void UIMode::render() {
    // Clear background
    SDL_SetRenderDrawColor(renderer_, BG_COLOR.r, BG_COLOR.g, BG_COLOR.b, BG_COLOR.a);
    SDL_RenderClear(renderer_);

    render_error_highlights();
    render_gutter();
    render_editor();
    render_cursor();
    render_status_bar();
}

void UIMode::render_gutter() {
    // Gutter background
    SDL_Rect gutter_rect = {0, 0, GUTTER_WIDTH, window_height_ - STATUS_HEIGHT};
    SDL_SetRenderDrawColor(renderer_, GUTTER_BG.r, GUTTER_BG.g, GUTTER_BG.b, GUTTER_BG.a);
    SDL_RenderFillRect(renderer_, &gutter_rect);

    // Line numbers
    int y = PADDING;
    int visible_lines = (window_height_ - STATUS_HEIGHT - PADDING * 2) / font_.line_height();
    std::size_t total_lines = buffer_.line_count();

    for (int i = 0; i < visible_lines && scroll_y_ + static_cast<std::size_t>(i) < total_lines; ++i) {
        std::size_t line_num = scroll_y_ + static_cast<std::size_t>(i) + 1;
        char num_str[8];
        std::snprintf(num_str, sizeof(num_str), "%3zu", line_num);

        int x = GUTTER_WIDTH - PADDING - font_.string_width(num_str);
        font_.draw_string(num_str, x, y, LINE_NUM_COLOR);

        y += font_.line_height();
    }
}

void UIMode::render_error_highlights() {
    if (error_lines_.empty()) return;

    int visible_lines = (window_height_ - STATUS_HEIGHT - PADDING * 2) / font_.line_height();

    for (std::size_t err_line : error_lines_) {
        if (err_line >= scroll_y_ && err_line < scroll_y_ + static_cast<std::size_t>(visible_lines)) {
            int y = PADDING + static_cast<int>(err_line - scroll_y_) * font_.line_height();
            SDL_Rect highlight = {GUTTER_WIDTH, y, window_width_ - GUTTER_WIDTH, font_.line_height()};
            SDL_SetRenderDrawColor(renderer_, ERROR_BG.r, ERROR_BG.g, ERROR_BG.b, ERROR_BG.a);
            SDL_RenderFillRect(renderer_, &highlight);
        }
    }
}

void UIMode::render_editor() {
    int y = PADDING;
    int x = GUTTER_WIDTH + PADDING;
    int visible_lines = (window_height_ - STATUS_HEIGHT - PADDING * 2) / font_.line_height();
    std::size_t total_lines = buffer_.line_count();

    for (int i = 0; i < visible_lines && scroll_y_ + static_cast<std::size_t>(i) < total_lines; ++i) {
        std::size_t line_idx = scroll_y_ + static_cast<std::size_t>(i);
        auto line = buffer_.line(line_idx);

        // Clip line to visible width
        int max_chars = (window_width_ - x - PADDING) / font_.char_width();
        std::size_t len = std::min(line.size(), static_cast<std::size_t>(max_chars));

        font_.draw_string(line.data(), len, x, y, TEXT_COLOR);

        y += font_.line_height();
    }
}

void UIMode::render_cursor() {
    if (!cursor_visible_) return;

    std::size_t cursor_line = buffer_.cursor_line();
    std::size_t cursor_col = buffer_.cursor_col();

    // Check if cursor is visible
    int visible_lines = (window_height_ - STATUS_HEIGHT - PADDING * 2) / font_.line_height();
    if (cursor_line < scroll_y_ || cursor_line >= scroll_y_ + static_cast<std::size_t>(visible_lines)) {
        return;
    }

    int x = GUTTER_WIDTH + PADDING + static_cast<int>(cursor_col) * font_.char_width();
    int y = PADDING + static_cast<int>(cursor_line - scroll_y_) * font_.line_height();

    // Draw cursor as a vertical line
    SDL_Rect cursor_rect = {x, y, 2, font_.line_height()};
    SDL_SetRenderDrawColor(renderer_, CURSOR_COLOR.r, CURSOR_COLOR.g, CURSOR_COLOR.b, CURSOR_COLOR.a);
    SDL_RenderFillRect(renderer_, &cursor_rect);
}

void UIMode::render_status_bar() {
    // Status bar background
    SDL_Rect status_rect = {0, window_height_ - STATUS_HEIGHT, window_width_, STATUS_HEIGHT};
    SDL_SetRenderDrawColor(renderer_, STATUS_BG.r, STATUS_BG.g, STATUS_BG.b, STATUS_BG.a);
    SDL_RenderFillRect(renderer_, &status_rect);

    // Status indicator dot
    SDL_Color dot_color = playing_ ? STATUS_PLAY :
                          error_lines_.empty() ? STATUS_OK : STATUS_ERR;
    SDL_Rect dot = {PADDING, window_height_ - STATUS_HEIGHT + (STATUS_HEIGHT - 8) / 2, 8, 8};
    SDL_SetRenderDrawColor(renderer_, dot_color.r, dot_color.g, dot_color.b, dot_color.a);
    SDL_RenderFillRect(renderer_, &dot);

    // Status text
    int text_x = PADDING + 16;
    int text_y = window_height_ - STATUS_HEIGHT + (STATUS_HEIGHT - font_.line_height()) / 2;

    // Truncate status message if too long
    int max_chars = (window_width_ - text_x - PADDING) / font_.char_width();
    std::string display_status = status_message_;
    if (static_cast<int>(display_status.size()) > max_chars && max_chars > 3) {
        display_status = display_status.substr(0, static_cast<std::size_t>(max_chars - 3)) + "...";
    }

    font_.draw_string(display_status.c_str(), text_x, text_y, TEXT_COLOR);
}

}  // namespace enkido::ui
