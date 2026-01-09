#include "text_buffer.hpp"
#include <algorithm>

namespace enkido::ui {

void TextBuffer::mark_dirty() {
    lines_dirty_ = true;
}

void TextBuffer::rebuild_line_cache() const {
    if (!lines_dirty_) return;

    line_starts_.clear();
    line_starts_.push_back(0);

    for (std::size_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\n') {
            line_starts_.push_back(i + 1);
        }
    }

    lines_dirty_ = false;
}

std::size_t TextBuffer::line_at_pos(std::size_t pos) const {
    rebuild_line_cache();

    // Binary search for line containing pos
    auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), pos);
    if (it == line_starts_.begin()) {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(line_starts_.begin(), it) - 1);
}

std::size_t TextBuffer::line_start(std::size_t line_idx) const {
    rebuild_line_cache();
    if (line_idx >= line_starts_.size()) {
        return text_.size();
    }
    return line_starts_[line_idx];
}

std::size_t TextBuffer::line_end(std::size_t line_idx) const {
    rebuild_line_cache();
    if (line_idx + 1 < line_starts_.size()) {
        return line_starts_[line_idx + 1] - 1;  // Before the newline
    }
    return text_.size();
}

void TextBuffer::insert_char(char c) {
    if (has_selection_) {
        delete_selection();
    }
    text_.insert(cursor_, 1, c);
    cursor_++;
    mark_dirty();
}

void TextBuffer::insert_text(std::string_view text) {
    if (has_selection_) {
        delete_selection();
    }
    text_.insert(cursor_, text);
    cursor_ += text.size();
    mark_dirty();
}

void TextBuffer::backspace() {
    if (has_selection_) {
        delete_selection();
        return;
    }
    if (cursor_ > 0) {
        text_.erase(cursor_ - 1, 1);
        cursor_--;
        mark_dirty();
    }
}

void TextBuffer::delete_char() {
    if (has_selection_) {
        delete_selection();
        return;
    }
    if (cursor_ < text_.size()) {
        text_.erase(cursor_, 1);
        mark_dirty();
    }
}

void TextBuffer::newline() {
    insert_char('\n');
}

void TextBuffer::move_left() {
    clear_selection();
    if (cursor_ > 0) {
        cursor_--;
    }
}

void TextBuffer::move_right() {
    clear_selection();
    if (cursor_ < text_.size()) {
        cursor_++;
    }
}

void TextBuffer::move_up() {
    clear_selection();
    std::size_t current_line = cursor_line();
    if (current_line == 0) {
        cursor_ = 0;
        return;
    }

    std::size_t col = cursor_col();
    std::size_t prev_line_start = line_start(current_line - 1);
    std::size_t prev_line_end = line_end(current_line - 1);
    std::size_t prev_line_len = prev_line_end - prev_line_start;

    cursor_ = prev_line_start + std::min(col, prev_line_len);
}

void TextBuffer::move_down() {
    clear_selection();
    std::size_t current_line = cursor_line();
    if (current_line + 1 >= line_count()) {
        cursor_ = text_.size();
        return;
    }

    std::size_t col = cursor_col();
    std::size_t next_line_start = line_start(current_line + 1);
    std::size_t next_line_end = line_end(current_line + 1);
    std::size_t next_line_len = next_line_end - next_line_start;

    cursor_ = next_line_start + std::min(col, next_line_len);
}

void TextBuffer::home() {
    clear_selection();
    std::size_t current_line = cursor_line();
    cursor_ = line_start(current_line);
}

void TextBuffer::end() {
    clear_selection();
    std::size_t current_line = cursor_line();
    cursor_ = line_end(current_line);
}

void TextBuffer::move_to_start() {
    clear_selection();
    cursor_ = 0;
}

void TextBuffer::move_to_end() {
    clear_selection();
    cursor_ = text_.size();
}

void TextBuffer::select_all() {
    selection_start_ = 0;
    selection_end_ = text_.size();
    has_selection_ = !text_.empty();
    cursor_ = text_.size();
}

void TextBuffer::clear_selection() {
    has_selection_ = false;
    selection_start_ = selection_end_ = cursor_;
}

bool TextBuffer::has_selection() const {
    return has_selection_ && selection_start_ != selection_end_;
}

void TextBuffer::delete_selection() {
    if (!has_selection()) return;

    std::size_t start = std::min(selection_start_, selection_end_);
    std::size_t end = std::max(selection_start_, selection_end_);

    text_.erase(start, end - start);
    cursor_ = start;
    clear_selection();
    mark_dirty();
}

std::string_view TextBuffer::get_selection() const {
    if (!has_selection()) return {};

    std::size_t start = std::min(selection_start_, selection_end_);
    std::size_t end = std::max(selection_start_, selection_end_);

    return std::string_view(text_).substr(start, end - start);
}

std::string_view TextBuffer::line(std::size_t n) const {
    rebuild_line_cache();
    if (n >= line_starts_.size()) {
        return {};
    }

    std::size_t start = line_starts_[n];
    std::size_t end;
    if (n + 1 < line_starts_.size()) {
        end = line_starts_[n + 1] - 1;  // Exclude newline
    } else {
        end = text_.size();
    }

    return std::string_view(text_).substr(start, end - start);
}

std::size_t TextBuffer::line_count() const {
    rebuild_line_cache();
    return line_starts_.size();
}

std::size_t TextBuffer::cursor_line() const {
    return line_at_pos(cursor_);
}

std::size_t TextBuffer::cursor_col() const {
    std::size_t current_line = cursor_line();
    std::size_t start = line_start(current_line);
    return cursor_ - start;
}

void TextBuffer::clear() {
    text_.clear();
    cursor_ = 0;
    clear_selection();
    mark_dirty();
}

void TextBuffer::set_text(std::string_view text) {
    text_ = text;
    cursor_ = text_.size();
    clear_selection();
    mark_dirty();
}

}  // namespace enkido::ui
