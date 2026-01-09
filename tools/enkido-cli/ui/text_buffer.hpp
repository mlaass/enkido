#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstddef>

namespace enkido::ui {

class TextBuffer {
public:
    TextBuffer() = default;

    // Text modification
    void insert_char(char c);
    void insert_text(std::string_view text);
    void backspace();
    void delete_char();
    void newline();

    // Cursor movement
    void move_left();
    void move_right();
    void move_up();
    void move_down();
    void home();
    void end();
    void move_to_start();
    void move_to_end();

    // Selection
    void select_all();
    void clear_selection();
    bool has_selection() const;
    void delete_selection();
    std::string_view get_selection() const;

    // Accessors
    [[nodiscard]] std::string_view text() const { return text_; }
    [[nodiscard]] std::string_view line(std::size_t n) const;
    [[nodiscard]] std::size_t line_count() const;
    [[nodiscard]] std::size_t cursor_line() const;
    [[nodiscard]] std::size_t cursor_col() const;
    [[nodiscard]] std::size_t cursor_pos() const { return cursor_; }

    // Clear all content
    void clear();

    // Set content (replaces everything)
    void set_text(std::string_view text);

private:
    std::string text_;
    std::size_t cursor_ = 0;
    std::size_t selection_start_ = 0;
    std::size_t selection_end_ = 0;
    bool has_selection_ = false;

    // Cached line positions (start index of each line)
    mutable std::vector<std::size_t> line_starts_;
    mutable bool lines_dirty_ = true;

    void rebuild_line_cache() const;
    void mark_dirty();

    // Find line containing position
    std::size_t line_at_pos(std::size_t pos) const;
    // Get start position of line
    std::size_t line_start(std::size_t line_idx) const;
    // Get end position of line (before newline or end of text)
    std::size_t line_end(std::size_t line_idx) const;
};

}  // namespace enkido::ui
