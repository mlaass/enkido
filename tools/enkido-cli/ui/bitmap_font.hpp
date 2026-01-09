#pragma once

#include <SDL2/SDL.h>
#include <cstdint>

namespace enkido::ui {

constexpr int GLYPH_WIDTH = 8;
constexpr int GLYPH_HEIGHT = 12;
constexpr int FONT_FIRST_CHAR = 32;   // Space
constexpr int FONT_LAST_CHAR = 126;   // Tilde
constexpr int FONT_NUM_CHARS = FONT_LAST_CHAR - FONT_FIRST_CHAR + 1;

class BitmapFont {
public:
    BitmapFont() = default;
    ~BitmapFont();

    BitmapFont(const BitmapFont&) = delete;
    BitmapFont& operator=(const BitmapFont&) = delete;

    bool init(SDL_Renderer* renderer);
    void shutdown();

    void draw_char(char c, int x, int y, SDL_Color color);
    void draw_string(const char* str, int x, int y, SDL_Color color);
    void draw_string(const char* str, std::size_t len, int x, int y, SDL_Color color);

    [[nodiscard]] int string_width(const char* str) const;
    [[nodiscard]] int char_width() const { return GLYPH_WIDTH; }
    [[nodiscard]] int line_height() const { return GLYPH_HEIGHT; }

private:
    SDL_Texture* texture_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
};

}  // namespace enkido::ui
