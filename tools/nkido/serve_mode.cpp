#include "serve_mode.hpp"

#include "asset_loader.hpp"
#include "audio_engine.hpp"
#include "program_loader.hpp"
#include "ui/bitmap_font.hpp"
#include "akkado/akkado.hpp"
#include "akkado/diagnostics.hpp"
#include "cedar/vm/vm.hpp"
#include "cedar/opcodes/dsp_state.hpp"
#include "cedar/platform/stdio_binary.hpp"

#include <SDL.h>
#include <algorithm>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nkido {

namespace {

// ----------------------------------------------------------------------------
// JSON output helpers
// ----------------------------------------------------------------------------

std::string escape_json(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        switch (u) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (u < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", u);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void emit(std::string_view line) {
    std::cout << line << '\n';
    std::cout.flush();
}

void emit_event(std::string_view name) {
    std::string out;
    out.reserve(32);
    out += R"({"event":")";
    out += name;
    out += R"("})";
    emit(out);
}

void emit_compiled(bool ok) {
    emit(ok
        ? std::string_view{R"({"event":"compiled","ok":true})"}
        : std::string_view{R"({"event":"compiled","ok":false})"});
}

void emit_diagnostic(const akkado::Diagnostic& diag) {
    // Reuse akkado::format_diagnostic_json for the body; splice in the event tag.
    std::string body = akkado::format_diagnostic_json(diag);
    if (body.size() < 2 || body.front() != '{') {
        return;  // malformed; skip
    }
    std::string out;
    out.reserve(body.size() + 32);
    out += R"({"event":"diagnostic",)";
    out.append(body, 1, body.size() - 1);  // drop leading '{'
    emit(out);
}

void emit_error(std::string_view msg) {
    std::string out;
    out.reserve(msg.size() + 32);
    out += R"({"event":"error","message":")";
    out += escape_json(msg);
    out += R"("})";
    emit(out);
}

void emit_param_changed(std::string_view name, float value, bool ok) {
    std::string out;
    out.reserve(96);
    out += R"({"event":"param_changed","name":")";
    out += escape_json(name);
    out += R"(","value":)";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(value));
    out += buf;
    out += R"(,"ok":)";
    out += ok ? "true" : "false";
    out += "}";
    emit(out);
}

void emit_bank_registered(std::string_view uri, std::size_t sample_count) {
    std::string out;
    out.reserve(uri.size() + 64);
    out += R"({"event":"bank_registered","uri":")";
    out += escape_json(uri);
    out += R"(","sample_count":)";
    out += std::to_string(sample_count);
    out += "}";
    emit(out);
}

// ----------------------------------------------------------------------------
// Tiny JSON object parser
// ----------------------------------------------------------------------------
//
// Parses a single top-level JSON object whose values are strings, numbers,
// booleans, or null. Sufficient for the serve-mode wire protocol; not a
// general-purpose parser. Supports the standard escape sequences used by
// JSON.stringify (\", \\, \/, \b, \f, \n, \r, \t, \uXXXX).

struct JsonValue {
    enum class Kind { String, Number, Bool, Null };
    Kind kind = Kind::Null;
    std::string str;
    double num = 0.0;
    bool boolean = false;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : src_(input), pos_(0) {}

    std::optional<std::unordered_map<std::string, JsonValue>> parse_object() {
        skip_ws();
        if (!consume('{')) return std::nullopt;
        std::unordered_map<std::string, JsonValue> out;
        skip_ws();
        if (peek() == '}') { ++pos_; return out; }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return std::nullopt;
            skip_ws();
            if (!consume(':')) return std::nullopt;
            skip_ws();
            JsonValue v;
            if (!parse_value(v)) return std::nullopt;
            out[std::move(key)] = std::move(v);
            skip_ws();
            char c = peek();
            if (c == ',') { ++pos_; continue; }
            if (c == '}') { ++pos_; break; }
            return std::nullopt;
        }
        return out;
    }

private:
    char peek() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }
    bool consume(char c) {
        if (peek() != c) return false;
        ++pos_;
        return true;
    }
    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    // Encode a Unicode code point as UTF-8 into out. Caller validates range.
    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parse_string(std::string& out) {
        if (!consume('"')) return false;
        out.clear();
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= src_.size()) return false;
                char e = src_[pos_++];
                switch (e) {
                    case '"':  out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (pos_ + 4 > src_.size()) return false;
                        std::uint32_t cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = src_[pos_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<std::uint32_t>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<std::uint32_t>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<std::uint32_t>(h - 'A' + 10);
                            else return false;
                        }
                        // Surrogate pair handling: if high surrogate and the
                        // following bytes are \uXXXX of low surrogate, decode
                        // to a single code point.
                        if (cp >= 0xD800 && cp <= 0xDBFF &&
                            pos_ + 6 <= src_.size() &&
                            src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                            std::uint32_t low = 0;
                            std::size_t save = pos_;
                            pos_ += 2;
                            for (int i = 0; i < 4; ++i) {
                                char h = src_[pos_++];
                                low <<= 4;
                                if (h >= '0' && h <= '9') low |= static_cast<std::uint32_t>(h - '0');
                                else if (h >= 'a' && h <= 'f') low |= static_cast<std::uint32_t>(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') low |= static_cast<std::uint32_t>(h - 'A' + 10);
                                else { pos_ = save; low = 0; break; }
                            }
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                pos_ = save;
                            }
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: return false;
                }
            } else {
                out += c;
            }
        }
        return false;  // unterminated
    }

    bool parse_value(JsonValue& v) {
        char c = peek();
        if (c == '"') {
            v.kind = JsonValue::Kind::String;
            return parse_string(v.str);
        }
        if (c == 't' || c == 'f') {
            std::string_view rest = src_.substr(pos_);
            if (rest.substr(0, 4) == "true") {
                pos_ += 4;
                v.kind = JsonValue::Kind::Bool;
                v.boolean = true;
                return true;
            }
            if (rest.substr(0, 5) == "false") {
                pos_ += 5;
                v.kind = JsonValue::Kind::Bool;
                v.boolean = false;
                return true;
            }
            return false;
        }
        if (c == 'n') {
            if (src_.substr(pos_, 4) == "null") {
                pos_ += 4;
                v.kind = JsonValue::Kind::Null;
                return true;
            }
            return false;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            std::size_t start = pos_;
            if (c == '-') ++pos_;
            while (pos_ < src_.size()) {
                char d = src_[pos_];
                if ((d >= '0' && d <= '9') || d == '.' || d == 'e' || d == 'E' ||
                    d == '+' || d == '-') ++pos_;
                else break;
            }
            try {
                v.num = std::stod(std::string(src_.substr(start, pos_ - start)));
            } catch (...) {
                return false;
            }
            v.kind = JsonValue::Kind::Number;
            return true;
        }
        return false;
    }

    std::string_view src_;
    std::size_t pos_;
};

}  // namespace

// ----------------------------------------------------------------------------
// SDL window: live waveform + level meter
// ----------------------------------------------------------------------------

constexpr int    WINDOW_W       = 560;
constexpr int    WINDOW_H       = 260;
constexpr int    TOOLBAR_H      = 36;
constexpr int    METER_H        = 14;
constexpr Uint32 SDL_USEREVENT_JSON_LINE = 1;  // user.code value

enum class DisplayMode { Waveform, Oscilloscope };

// Per-column min/max envelope history that scrolls right→left over a fixed
// duration (5 s by default). New audio is folded into an accumulator; each
// time durationMs/numColumns has elapsed we commit the accumulator as a new
// column and slide. Mirrors the behavior of web/src/lib/visualizations/waveform.ts.
struct WaveformViewState {
    std::vector<float> hist_min;          // size = num_columns
    std::vector<float> hist_max;
    int     num_columns       = 0;
    int     write_pos         = 0;
    int     filled_columns    = 0;
    Uint64  last_column_time_ms = 0;
    float   acc_min           = 1e30f;
    float   acc_max           = -1e30f;
    bool    acc_any           = false;
    int     duration_ms       = 5000;
    // Smoothed display bounds. Snap-up on expansion, decay slowly toward
    // smaller targets so transient peaks don't permanently zoom out.
    float   display_min       = -1.0f;
    float   display_max       = 1.0f;
};

struct OscilloscopeViewState {
    float display_min = -1.0f;
    float display_max = 1.0f;
};

constexpr float SCALE_DECAY = 0.03f;

struct ToolbarLayout {
    SDL_Rect toolbar;
    SDL_Rect play_btn;
    SDL_Rect mode_btn;       // outer rect spanning Wave|Osc
    SDL_Rect mode_wave_half; // left half (Wave)
    SDL_Rect mode_osc_half;  // right half (Osc)
    SDL_Rect bpm_label;      // "120 BPM" readout
    SDL_Rect slider_track;
    SDL_Rect readout;
    int      slider_x_min;   // pixel range the thumb can occupy
    int      slider_x_max;
};

ToolbarLayout compute_layout(int w, int h) {
    ToolbarLayout L{};
    L.toolbar = {0, h - TOOLBAR_H, w, TOOLBAR_H};

    const int pad   = 6;
    const int btn_y = L.toolbar.y + 4;
    const int btn_h = TOOLBAR_H - 8;

    L.play_btn = {pad, btn_y, btn_h, btn_h};   // square

    const int mode_w = 80;
    L.mode_btn = {L.play_btn.x + L.play_btn.w + pad, btn_y, mode_w, btn_h};
    L.mode_wave_half = {L.mode_btn.x, L.mode_btn.y, mode_w / 2, btn_h};
    L.mode_osc_half  = {L.mode_btn.x + mode_w / 2, L.mode_btn.y, mode_w - mode_w / 2, btn_h};

    const int readout_w = 44;
    L.readout = {w - readout_w - pad, btn_y, readout_w, btn_h};

    const int bpm_w = 64;
    L.bpm_label = {L.mode_btn.x + L.mode_btn.w + pad, btn_y, bpm_w, btn_h};

    const int track_x = L.bpm_label.x + L.bpm_label.w + pad;
    const int track_w = L.readout.x - pad - track_x;
    const int track_h = 4;
    L.slider_track = {track_x, btn_y + (btn_h - track_h) / 2, track_w, track_h};
    L.slider_x_min = track_x;
    L.slider_x_max = track_x + track_w;
    return L;
}

float volume_from_x(int x, const ToolbarLayout& L) {
    if (L.slider_x_max <= L.slider_x_min) return 1.0f;
    float t = static_cast<float>(x - L.slider_x_min) /
              static_cast<float>(L.slider_x_max - L.slider_x_min);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * 1.5f;  // 0..150%
}

int slider_thumb_x(float volume, const ToolbarLayout& L) {
    float t = volume / 1.5f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return L.slider_x_min + static_cast<int>(t *
        static_cast<float>(L.slider_x_max - L.slider_x_min));
}

bool point_in_rect(int x, int y, const SDL_Rect& r) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Snap-up / decay-down auto-scale, floored at ±1 so the unity reference is
// always on screen. Mirrors updateDisplayRange() in the web visualizations.
void update_display_range(float& display_min, float& display_max,
                          float obs_min, float obs_max) {
    float target_max = (obs_max > 1.0f) ? obs_max : 1.0f;
    float target_min = (obs_min < -1.0f) ? obs_min : -1.0f;
    display_max = (target_max > display_max)
        ? target_max
        : display_max + (target_max - display_max) * SCALE_DECAY;
    display_min = (target_min < display_min)
        ? target_min
        : display_min + (target_min - display_min) * SCALE_DECAY;
}

// Draw the chrome shared by both views: zero line and dashed ±1 reference.
// Returns the y coordinates of zero, +1, and -1 for the caller to use when
// deciding what to overdraw red.
void draw_chrome(SDL_Renderer* ren, SDL_Rect area,
                 float display_min, float display_max,
                 int& y_zero, int& y_plus_one, int& y_minus_one) {
    const float margin = std::max(2.0f, static_cast<float>(area.h) * 0.05f);
    const float usable = static_cast<float>(area.h) - 2.0f * margin;
    const float range  = std::max(1e-9f, display_max - display_min);
    auto value_to_y = [&](float v) -> int {
        return area.y + static_cast<int>(margin +
            ((display_max - v) / range) * usable);
    };
    y_zero      = value_to_y(0.0f);
    y_plus_one  = value_to_y(1.0f);
    y_minus_one = value_to_y(-1.0f);

    // Zero line.
    SDL_SetRenderDrawColor(ren, 60, 60, 60, 255);
    SDL_RenderDrawLine(ren, area.x, y_zero, area.x + area.w - 1, y_zero);

    // ±1 unity boundary lines, dashed.
    SDL_SetRenderDrawColor(ren, 50, 50, 50, 255);
    for (int x = area.x; x < area.x + area.w; x += 7) {
        int xe = std::min(x + 4, area.x + area.w - 1);
        SDL_RenderDrawLine(ren, x, y_plus_one,  xe, y_plus_one);
        SDL_RenderDrawLine(ren, x, y_minus_one, xe, y_minus_one);
    }
}

// Long-horizon scrolling envelope (~5 s by default). Each pixel column shows
// the [min, max] envelope of all samples that fell into that time slice.
void render_waveform(SDL_Renderer* ren, const float* buf, std::size_t N,
                     SDL_Rect area, bool playing, WaveformViewState& s) {
    if (area.w < 2 || area.h < 4) return;

    // (Re)allocate the per-column history when the visible width changes.
    if (s.num_columns != area.w) {
        s.num_columns       = area.w;
        s.hist_min.assign(static_cast<std::size_t>(area.w), 0.0f);
        s.hist_max.assign(static_cast<std::size_t>(area.w), 0.0f);
        s.write_pos         = 0;
        s.filled_columns    = 0;
        s.acc_min           = 1e30f;
        s.acc_max           = -1e30f;
        s.acc_any           = false;
        s.last_column_time_ms = SDL_GetTicks64();
    }

    if (playing && N > 0) {
        for (std::size_t i = 0; i < N; ++i) {
            if (buf[i] < s.acc_min) s.acc_min = buf[i];
            if (buf[i] > s.acc_max) s.acc_max = buf[i];
        }
        s.acc_any = true;
    }

    const Uint64 now_ms = SDL_GetTicks64();
    const float ms_per_col = static_cast<float>(s.duration_ms) /
                             static_cast<float>(s.num_columns);
    int owed = static_cast<int>(
        static_cast<float>(now_ms - s.last_column_time_ms) / ms_per_col);
    if (owed > s.num_columns) owed = s.num_columns;
    if (owed > 0) {
        const float mn = s.acc_any ? s.acc_min : 0.0f;
        const float mx = s.acc_any ? s.acc_max : 0.0f;
        for (int i = 0; i < owed; ++i) {
            s.hist_min[static_cast<std::size_t>(s.write_pos)] = mn;
            s.hist_max[static_cast<std::size_t>(s.write_pos)] = mx;
            s.write_pos = (s.write_pos + 1) % s.num_columns;
            if (s.filled_columns < s.num_columns) ++s.filled_columns;
        }
        s.last_column_time_ms += static_cast<Uint64>(
            static_cast<float>(owed) * ms_per_col);
        s.acc_min = 1e30f;
        s.acc_max = -1e30f;
        s.acc_any = false;
    }

    // Auto-scale viewport from the visible history.
    float obs_min = 0.0f, obs_max = 0.0f;
    if (s.filled_columns > 0) {
        obs_min = 1e30f; obs_max = -1e30f;
        for (int i = 0; i < s.filled_columns; ++i) {
            const auto idx = static_cast<std::size_t>(i);
            if (s.hist_min[idx] < obs_min) obs_min = s.hist_min[idx];
            if (s.hist_max[idx] > obs_max) obs_max = s.hist_max[idx];
        }
    }
    update_display_range(s.display_min, s.display_max, obs_min, obs_max);

    int y_zero, y_plus_one, y_minus_one;
    draw_chrome(ren, area, s.display_min, s.display_max,
                y_zero, y_plus_one, y_minus_one);

    if (s.filled_columns == 0) return;

    const int oldest = (s.filled_columns < s.num_columns) ? 0 : s.write_pos;
    const float margin = std::max(2.0f, static_cast<float>(area.h) * 0.05f);
    const float usable = static_cast<float>(area.h) - 2.0f * margin;
    const float range  = std::max(1e-9f, s.display_max - s.display_min);
    auto value_to_y = [&](float v) -> int {
        int y = area.y + static_cast<int>(margin +
            ((s.display_max - v) / range) * usable);
        if (y < area.y) y = area.y;
        if (y >= area.y + area.h) y = area.y + area.h - 1;
        return y;
    };

    SDL_Color in_color  = playing ? SDL_Color{80, 220, 100, 255}
                                   : SDL_Color{70, 70, 70, 255};
    SDL_Color err_color{248, 81, 73, 255};

    for (int x = 0; x < area.w; ++x) {
        const int read_offset = static_cast<int>(
            (static_cast<float>(x) / static_cast<float>(std::max(1, area.w - 1))) *
            static_cast<float>(s.filled_columns - 1));
        const int hist_idx = (oldest + read_offset) % s.num_columns;
        const auto idx = static_cast<std::size_t>(hist_idx);
        const float mn = s.hist_min[idx];
        const float mx = s.hist_max[idx];
        if (mn == 0.0f && mx == 0.0f) continue;

        const int y_top = value_to_y(mx);  // smaller y (top of bar)
        const int y_bot = value_to_y(mn);

        // In-band portion (within ±1).
        const int in_top = std::max(y_top, y_plus_one);
        const int in_bot = std::min(y_bot, y_minus_one);
        if (in_bot > in_top) {
            SDL_SetRenderDrawColor(ren, in_color.r, in_color.g, in_color.b, 255);
            SDL_RenderDrawLine(ren, area.x + x, in_top, area.x + x, in_bot);
        }
        // Above +1: y < y_plus_one.
        if (y_top < y_plus_one) {
            int top = y_top;
            int bot = std::min(y_plus_one, y_bot);
            if (bot > top) {
                SDL_SetRenderDrawColor(ren, err_color.r, err_color.g, err_color.b, 255);
                SDL_RenderDrawLine(ren, area.x + x, top, area.x + x, bot);
            }
        }
        // Below -1: y > y_minus_one.
        if (y_bot > y_minus_one) {
            int top = std::max(y_minus_one, y_top);
            int bot = y_bot;
            if (bot > top) {
                SDL_SetRenderDrawColor(ren, err_color.r, err_color.g, err_color.b, 255);
                SDL_RenderDrawLine(ren, area.x + x, top, area.x + x, bot);
            }
        }
    }
}

// Short rolling window of raw samples, locked to a rising zero-crossing for
// a stable phase reference. ~21 ms at 48 kHz when fed 1024 samples.
void render_oscilloscope(SDL_Renderer* ren, const float* buf, std::size_t N,
                         SDL_Rect area, bool playing,
                         OscilloscopeViewState& s) {
    if (N < 4 || area.w < 2 || area.h < 4) return;

    // Trigger search: rising zero-crossing in the first quarter so the
    // remaining 3/4 of the buffer is plot data.
    const std::size_t search_end = N / 4;
    std::size_t trigger = 0;
    for (std::size_t i = 1; i < search_end; ++i) {
        if (buf[i - 1] < 0.0f && buf[i] >= 0.0f) {
            trigger = i;
            break;
        }
    }
    const std::size_t plot_n = N - trigger;
    const float* plot = buf + trigger;

    // Smoothed viewport — same algorithm as the web oscilloscope.
    float obs_min = 1e30f, obs_max = -1e30f;
    for (std::size_t i = 0; i < plot_n; ++i) {
        if (plot[i] < obs_min) obs_min = plot[i];
        if (plot[i] > obs_max) obs_max = plot[i];
    }
    if (!playing) { obs_min = 0.0f; obs_max = 0.0f; }
    update_display_range(s.display_min, s.display_max, obs_min, obs_max);

    int y_zero, y_plus_one, y_minus_one;
    draw_chrome(ren, area, s.display_min, s.display_max,
                y_zero, y_plus_one, y_minus_one);

    const float margin = std::max(2.0f, static_cast<float>(area.h) * 0.05f);
    const float usable = static_cast<float>(area.h) - 2.0f * margin;
    const float range  = std::max(1e-9f, s.display_max - s.display_min);
    auto value_to_y = [&](float v) -> int {
        int y = area.y + static_cast<int>(margin +
            ((s.display_max - v) / range) * usable);
        if (y < area.y) y = area.y;
        if (y >= area.y + area.h) y = area.y + area.h - 1;
        return y;
    };

    // Pre-compute y for every column.
    std::vector<int> ys(static_cast<std::size_t>(area.w));
    const float step = static_cast<float>(plot_n) / static_cast<float>(area.w);
    for (int x = 0; x < area.w; ++x) {
        std::size_t si = static_cast<std::size_t>(static_cast<float>(x) * step);
        if (si >= plot_n) si = plot_n - 1;
        ys[static_cast<std::size_t>(x)] = value_to_y(plot[si]);
    }

    // Pass 1: full waveform.
    if (playing) SDL_SetRenderDrawColor(ren, 80, 220, 100, 255);
    else         SDL_SetRenderDrawColor(ren, 70, 70, 70, 255);
    for (int x = 1; x < area.w; ++x) {
        SDL_RenderDrawLine(ren,
            area.x + x - 1, ys[static_cast<std::size_t>(x - 1)],
            area.x + x,     ys[static_cast<std::size_t>(x)]);
    }

    // Pass 2: overdraw segments outside ±1 in red.
    SDL_SetRenderDrawColor(ren, 248, 81, 73, 255);
    for (int x = 1; x < area.w; ++x) {
        int y0 = ys[static_cast<std::size_t>(x - 1)];
        int y1 = ys[static_cast<std::size_t>(x)];
        bool clipped = (y0 < y_plus_one || y0 > y_minus_one ||
                        y1 < y_plus_one || y1 > y_minus_one);
        if (clipped) {
            SDL_RenderDrawLine(ren, area.x + x - 1, y0, area.x + x, y1);
        }
    }
}

void render_level_meter(SDL_Renderer* ren, float peak, SDL_Rect area) {
    SDL_SetRenderDrawColor(ren, 30, 30, 30, 255);
    SDL_RenderFillRect(ren, &area);

    const bool over = peak > 1.0f;
    const float clamped = std::min(peak, 1.0f);
    int bar_w = static_cast<int>(clamped * static_cast<float>(area.w));
    if (bar_w > area.w) bar_w = area.w;

    if (bar_w > 0) {
        SDL_Rect bar{area.x, area.y, bar_w, area.h};
        if (over)               SDL_SetRenderDrawColor(ren, 248, 81, 73, 255);
        else if (peak > 0.01f)  SDL_SetRenderDrawColor(ren, 80, 220, 100, 255);
        else                    SDL_SetRenderDrawColor(ren, 60, 90, 60, 255);
        SDL_RenderFillRect(ren, &bar);
    }

    // Persistent overload tip on the right edge when peak > 1.
    if (over) {
        SDL_Rect tip{area.x + area.w - 4, area.y, 4, area.h};
        SDL_SetRenderDrawColor(ren, 248, 81, 73, 255);
        SDL_RenderFillRect(ren, &tip);
    }
}

void fill_triangle_play(SDL_Renderer* ren, SDL_Rect r, SDL_Color c) {
    // Filled right-pointing triangle inside r, drawn as horizontal scanlines.
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    const int pad_x = r.w / 4;
    const int pad_y = r.h / 5;
    const int x0 = r.x + pad_x;
    const int y0 = r.y + pad_y;
    const int y1 = r.y + r.h - pad_y;
    const int x1 = r.x + r.w - pad_x;
    const int half = (y1 - y0) / 2;
    for (int dy = 0; dy <= y1 - y0; ++dy) {
        int width;
        if (dy <= half) width = (x1 - x0) * dy / std::max(1, half);
        else            width = (x1 - x0) * (y1 - y0 - dy) / std::max(1, half);
        SDL_RenderDrawLine(ren, x0, y0 + dy, x0 + width, y0 + dy);
    }
}

void render_toolbar(SDL_Renderer* ren, ui::BitmapFont* font,
                    const ToolbarLayout& L, DisplayMode mode,
                    bool playing, bool has_program, float volume,
                    float bpm) {
    // Toolbar background.
    SDL_SetRenderDrawColor(ren, 22, 22, 26, 255);
    SDL_RenderFillRect(ren, &L.toolbar);
    SDL_SetRenderDrawColor(ren, 50, 50, 56, 255);
    SDL_RenderDrawLine(ren, L.toolbar.x, L.toolbar.y,
                       L.toolbar.x + L.toolbar.w - 1, L.toolbar.y);

    // Play / stop button.
    SDL_Color btn_bg{40, 40, 46, 255};
    SDL_Color btn_border{80, 80, 90, 255};
    if (!has_program) {
        btn_bg = {30, 30, 32, 255};
        btn_border = {50, 50, 54, 255};
    }
    SDL_SetRenderDrawColor(ren, btn_bg.r, btn_bg.g, btn_bg.b, 255);
    SDL_RenderFillRect(ren, &L.play_btn);
    SDL_SetRenderDrawColor(ren, btn_border.r, btn_border.g, btn_border.b, 255);
    SDL_RenderDrawRect(ren, &L.play_btn);

    SDL_Color icon = has_program ? SDL_Color{220, 220, 220, 255}
                                  : SDL_Color{90, 90, 90, 255};
    if (playing) {
        // Stop icon: filled square inset.
        const int inset_x = L.play_btn.w / 4;
        const int inset_y = L.play_btn.h / 4;
        SDL_Rect stop{L.play_btn.x + inset_x, L.play_btn.y + inset_y,
                      L.play_btn.w - 2 * inset_x, L.play_btn.h - 2 * inset_y};
        SDL_SetRenderDrawColor(ren, icon.r, icon.g, icon.b, 255);
        SDL_RenderFillRect(ren, &stop);
    } else {
        fill_triangle_play(ren, L.play_btn, icon);
    }

    // Mode segmented toggle.
    SDL_SetRenderDrawColor(ren, 40, 40, 46, 255);
    SDL_RenderFillRect(ren, &L.mode_btn);

    SDL_Rect selected = (mode == DisplayMode::Waveform)
        ? L.mode_wave_half : L.mode_osc_half;
    SDL_SetRenderDrawColor(ren, 60, 90, 140, 255);
    SDL_RenderFillRect(ren, &selected);

    SDL_SetRenderDrawColor(ren, 80, 80, 90, 255);
    SDL_RenderDrawRect(ren, &L.mode_btn);
    // Divider between halves.
    SDL_RenderDrawLine(ren,
        L.mode_wave_half.x + L.mode_wave_half.w, L.mode_btn.y,
        L.mode_wave_half.x + L.mode_wave_half.w, L.mode_btn.y + L.mode_btn.h);

    if (font) {
        SDL_Color text{220, 220, 220, 255};
        const char* w_label = "WAV";
        const char* o_label = "OSC";
        int w_lab_w = font->string_width(w_label);
        int o_lab_w = font->string_width(o_label);
        int text_y = L.mode_btn.y + (L.mode_btn.h - font->line_height()) / 2;
        font->draw_string(w_label,
            L.mode_wave_half.x + (L.mode_wave_half.w - w_lab_w) / 2,
            text_y, text);
        font->draw_string(o_label,
            L.mode_osc_half.x + (L.mode_osc_half.w - o_lab_w) / 2,
            text_y, text);
    }

    // BPM readout, left of the slider.
    if (font) {
        char buf[20];
        std::snprintf(buf, sizeof(buf), "%d BPM",
                      static_cast<int>(bpm + 0.5f));
        SDL_Color text{200, 200, 200, 255};
        int tw = font->string_width(buf);
        int ty = L.bpm_label.y + (L.bpm_label.h - font->line_height()) / 2;
        font->draw_string(buf, L.bpm_label.x + L.bpm_label.w - tw, ty, text);
    }

    // Slider track.
    SDL_SetRenderDrawColor(ren, 50, 50, 56, 255);
    SDL_RenderFillRect(ren, &L.slider_track);

    // Filled portion up to thumb.
    int thumb_x = slider_thumb_x(volume, L);
    SDL_Rect filled{L.slider_track.x, L.slider_track.y,
                    thumb_x - L.slider_track.x, L.slider_track.h};
    if (filled.w > 0) {
        SDL_SetRenderDrawColor(ren, 80, 220, 100, 255);
        SDL_RenderFillRect(ren, &filled);
    }

    // Slider thumb.
    const int thumb_w = 8;
    const int thumb_h = L.slider_track.h + 12;
    SDL_Rect thumb{thumb_x - thumb_w / 2,
                   L.slider_track.y + L.slider_track.h / 2 - thumb_h / 2,
                   thumb_w, thumb_h};
    SDL_SetRenderDrawColor(ren, 220, 220, 220, 255);
    SDL_RenderFillRect(ren, &thumb);
    SDL_SetRenderDrawColor(ren, 100, 100, 110, 255);
    SDL_RenderDrawRect(ren, &thumb);

    // Readout.
    if (font) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%",
                      static_cast<int>(volume * 100.0f + 0.5f));
        SDL_Color text{200, 200, 200, 255};
        int tw = font->string_width(buf);
        int ty = L.readout.y + (L.readout.h - font->line_height()) / 2;
        font->draw_string(buf, L.readout.x + L.readout.w - tw, ty, text);
    }
}

struct RenderInputs {
    AudioEngine*           engine;
    bool                   playing;
    bool                   has_program;
    DisplayMode            mode;
    float                  volume;
    float                  bpm;
    WaveformViewState*     wave_state;
    OscilloscopeViewState* osc_state;
};

void render_frame(SDL_Renderer* ren, ui::BitmapFont* font,
                  const RenderInputs& in) {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren, &w, &h);

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    constexpr std::size_t N = 1024;
    float buf[N]{};
    in.engine->get_waveform(buf, N);

    float peak = 0.0f;
    for (std::size_t i = 0; i < N; ++i) {
        float a = std::fabs(buf[i]);
        if (a > peak) peak = a;
    }

    ToolbarLayout L = compute_layout(w, h);

    // Layout: visualization region above level meter above toolbar.
    SDL_Rect viz{0, 0, w, h - TOOLBAR_H - METER_H};
    if (viz.h < 1) viz.h = 1;
    SDL_Rect meter{0, viz.y + viz.h, w, METER_H};

    if (in.mode == DisplayMode::Oscilloscope) {
        render_oscilloscope(ren, buf, N, viz, in.playing, *in.osc_state);
    } else {
        render_waveform(ren, buf, N, viz, in.playing, *in.wave_state);
    }
    render_level_meter(ren, peak, meter);
    render_toolbar(ren, font, L, in.mode, in.playing, in.has_program, in.volume, in.bpm);
}

// ----------------------------------------------------------------------------
// Command dispatch (factored out so the SDL event loop can call it)
// ----------------------------------------------------------------------------

struct ServeState {
    AudioEngine*              engine;
    AudioEngine::Config       audio_config;
    bool                      audio_initialized = false;
    bool                      playing           = false;
    bool                      should_quit       = false;

    // True once any program has compiled and loaded successfully. The window's
    // play button uses this to decide whether resuming is meaningful — without
    // a program, hitting play would just make silence.
    bool                      has_program       = false;

    DisplayMode               display_mode      = DisplayMode::Waveform;
    float                     master_volume     = 1.0f;
    bool                      slider_dragging   = false;

    WaveformViewState         wave_view;
    OscilloscopeViewState     osc_view;

    // Original Options carried in so --bank/--soundfont/--sample (URI Resolver
    // PRD §4.5) flags propagate from the CLI invocation into per-load asset
    // resolution.
    Options                   opts;

    // Backing memory for SequenceProgram inits, one entry per successful
    // hot-swap. Append-only — the audio thread may still hold pointers into
    // a previous program's events during the crossfade window. Memory grows
    // linearly with edit count (kilobytes per swap) which is fine for an
    // editor session.
    std::vector<std::vector<std::vector<cedar::Sequence>>> seq_storage_history;
};

// Hard-stops the audio device (matches `cmd:stop` semantics). Tearing down
// rather than pausing avoids the pulseaudio/pipewire-pulse cork issue noted
// in the original `cmd:stop` handler.
void stop_audio(ServeState& s) {
    if (s.playing) {
        s.engine->stop();
        s.audio_initialized = false;
        s.playing = false;
    }
}

// Resumes after a soft stop, reusing the program already in the VM. No
// recompile needed — the VM keeps the bytecode after `engine->stop()`.
// Returns false if there is nothing to play yet.
bool start_audio(ServeState& s) {
    if (s.playing) return true;
    if (!s.has_program) return false;
    if (!s.audio_initialized) {
        if (!s.engine->init(s.audio_config)) return false;
        s.audio_initialized = true;
    }
    s.engine->set_master_volume(s.master_volume);
    if (!s.engine->start()) return false;
    s.playing = true;
    return true;
}

void handle_command_line(ServeState& s, const std::string& line) {
    if (line.empty()) return;

    JsonParser parser(line);
    auto obj_opt = parser.parse_object();
    if (!obj_opt) {
        emit_error("invalid JSON");
        return;
    }
    const auto& obj = *obj_opt;

    auto cmd_it = obj.find("cmd");
    if (cmd_it == obj.end() || cmd_it->second.kind != JsonValue::Kind::String) {
        emit_error("missing 'cmd'");
        return;
    }
    const std::string& cmd = cmd_it->second.str;

    if (cmd == "load") {
        auto src_it = obj.find("source");
        if (src_it == obj.end() || src_it->second.kind != JsonValue::Kind::String) {
            emit_error("'load' requires 'source' (string)");
            emit_compiled(false);
            return;
        }
        std::string filename = "<vscode>";
        if (auto uri_it = obj.find("uri");
            uri_it != obj.end() && uri_it->second.kind == JsonValue::Kind::String) {
            filename = uri_it->second.str;
        }

        LoadResult load = compile_source(src_it->second.str, filename);
        if (!load.success) {
            if (load.compile_result) {
                for (const auto& diag : load.compile_result->diagnostics) {
                    emit_diagnostic(diag);
                }
            }
            emit_compiled(false);
            return;
        }
        // compile_source always populates compile_result on success.
        auto& cr = *load.compile_result;

        if (s.playing) {
            // Hot-swap: register any new assets first so the new program sees
            // them, then crossfade in the new bytecode, then write the new
            // program's state inits into a freshly allocated storage slot.
            if (!prepare_program_assets(s.engine->vm(), s.opts, cr, std::cerr)) {
                emit_error("asset load failed");
                emit_compiled(false);
                return;
            }
            resolve_sample_ids_in_events(s.engine->vm(), cr);
            // Grow the buffer pool to fit the new program's peak before
            // the crossfade arms. Runs on the compile/serve thread; the
            // chunked BufferPool keeps existing slab pointers stable so
            // the audio thread's in-flight reads of the previous program
            // remain valid.
            if (cr.required_buffers > 0) {
                s.engine->vm().buffers().ensure_capacity(cr.required_buffers);
            }
            // PRD L3: stage the FOREACH_EVENT subprogram table for this hot-swap.
            s.engine->vm().set_block_table(cr.block_table, cr.main_instruction_count);
            auto load_result = s.engine->vm().load_program(load.instructions);
            if (load_result != cedar::VM::LoadResult::Success) {
                const char* reason = "load failed";
                switch (load_result) {
                    case cedar::VM::LoadResult::SlotBusy: reason = "VM busy (try again)"; break;
                    case cedar::VM::LoadResult::TooLarge: reason = "program too large"; break;
                    default: break;
                }
                emit_error(reason);
                emit_compiled(false);
                return;
            }
            s.seq_storage_history.emplace_back();
            apply_state_inits(s.engine->vm(), cr, s.seq_storage_history.back());
            apply_builtin_var_overrides(s.engine->vm(), cr);
            s.engine->apply_midi_route_plan(cr.required_midi_sources, cr.required_midi_cc_routes);
        } else {
            if (!s.audio_initialized) {
                if (!s.engine->init(s.audio_config)) {
                    emit_error("failed to initialize audio");
                    emit_compiled(false);
                    return;
                }
                s.audio_initialized = true;
            }
            s.seq_storage_history.emplace_back();
            if (!load_and_prepare_immediate(s.engine->vm(), s.opts, load,
                                            s.seq_storage_history.back(), std::cerr)) {
                emit_error("failed to load program");
                emit_compiled(false);
                return;
            }
            s.engine->apply_midi_route_plan(cr.required_midi_sources, cr.required_midi_cc_routes);
            s.engine->set_master_volume(s.master_volume);
            if (!s.engine->start()) {
                emit_error("failed to start audio");
                emit_compiled(false);
                return;
            }
            s.playing = true;
        }
        s.has_program = true;
        emit_compiled(true);

    } else if (cmd == "stop") {
        stop_audio(s);
        emit_event("stopped");

    } else if (cmd == "set_volume") {
        auto value_it = obj.find("value");
        if (value_it == obj.end() || value_it->second.kind != JsonValue::Kind::Number) {
            emit_error("'set_volume' requires 'value' (number)");
            return;
        }
        float v = static_cast<float>(value_it->second.num);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.5f) v = 1.5f;
        s.master_volume = v;
        s.engine->set_master_volume(v);

    } else if (cmd == "set_param") {
        auto name_it = obj.find("name");
        auto value_it = obj.find("value");
        if (name_it == obj.end() || name_it->second.kind != JsonValue::Kind::String ||
            value_it == obj.end() || value_it->second.kind != JsonValue::Kind::Number) {
            emit_error("'set_param' requires 'name' (string) and 'value' (number)");
            return;
        }
        const float v = static_cast<float>(value_it->second.num);
        const bool ok = s.engine->vm().set_param(name_it->second.str.c_str(), v);
        emit_param_changed(name_it->second.str, v, ok);

    } else if (cmd == "load_bank") {
        // Register a sample-bank manifest URI with the same machinery `--bank`
        // uses on the CLI. The VS Code "Nkido extension" needs this because it
        // spawns `nkido serve` without flags; without this command, sample
        // patches typed in the editor produce only "sample 'X' not found in
        // any loaded bank" warnings.
        //
        // The URI is appended to `s.opts.bank_uris` so every subsequent `load`
        // command picks it up via prepare_program_assets(). We also fetch the
        // manifest immediately so the caller gets synchronous success/failure
        // feedback (and so the CLI logs "Loaded bank manifest 'X' from ..."
        // exactly once, when registered, instead of on every subsequent load).
        auto uri_it = obj.find("uri");
        if (uri_it == obj.end() || uri_it->second.kind != JsonValue::Kind::String) {
            emit_error("'load_bank' requires 'uri' (string)");
            return;
        }
        const std::string& uri = uri_it->second.str;
        auto& uris = s.opts.bank_uris;
        if (std::find(uris.begin(), uris.end(), uri) != uris.end()) {
            // Already registered — re-emit the event so the caller can treat
            // load_bank as idempotent.
            emit_bank_registered(uri, 0);
            return;
        }
        try {
            auto manifest = fetch_bank_manifest(uri);
            const std::size_t sample_count = manifest.samples.size();
            uris.push_back(uri);
            std::cerr << "Loaded bank manifest '" << uri
                      << "' (" << sample_count << " samples)\n";
            emit_bank_registered(uri, sample_count);
        } catch (const std::exception& e) {
            emit_error(std::string("bank '") + uri + "' failed to load: " + e.what());
        }

    } else if (cmd == "quit") {
        s.should_quit = true;

    } else {
        emit_error("unknown cmd");
    }
}

// ----------------------------------------------------------------------------
// Public entry point
// ----------------------------------------------------------------------------

int run_serve_mode(const Options& opts) {
    // Heap-allocate: AudioEngine holds a cedar::VM by value, which is large
    // enough to overflow the default thread stack in some build configurations.
    auto engine = std::make_unique<AudioEngine>();
    ServeState state;
    state.engine       = engine.get();
    state.audio_config = AudioEngine::Config{opts.sample_rate, opts.buffer_size, 2};
    state.opts         = opts;
    // Record the preferred MIDI input device so apply_midi_route_plan can
    // resolve bare midi() / DefaultDevice entries. Empty/null = first available.
    engine->set_preferred_midi_device(
        opts.midi_device ? opts.midi_device->c_str() : nullptr);

    // Switch stdin/stdout to binary so the line-delimited JSON protocol
    // is not corrupted by Windows CRT CRLF translation. POSIX: no-op.
    cedar::platform::set_stdio_binary_mode();
    install_signal_handlers();

    // Open an SDL window with a live waveform + level meter so the user
    // gets visual confirmation that audio is being produced. Window failure
    // is non-fatal — we fall back to headless serve.
    SDL_Window*   win = nullptr;
    SDL_Renderer* ren = nullptr;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0) {
        win = SDL_CreateWindow("nkido serve",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            WINDOW_W, WINDOW_H,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (win) {
            ren = SDL_CreateRenderer(win, -1,
                SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!ren) {
                std::fprintf(stderr, "warning: SDL_CreateRenderer failed: %s\n",
                             SDL_GetError());
                SDL_DestroyWindow(win);
                win = nullptr;
            }
        } else {
            std::fprintf(stderr, "warning: SDL_CreateWindow failed: %s\n",
                         SDL_GetError());
        }
    } else {
        std::fprintf(stderr, "warning: SDL_INIT_VIDEO failed: %s\n",
                     SDL_GetError());
    }

    // Bitmap font for toolbar labels. Failure is non-fatal — toolbar still
    // draws icons + slider, just without text.
    ui::BitmapFont font;
    bool font_ok = false;
    if (ren) {
        font_ok = font.init(ren);
        if (!font_ok) {
            std::fprintf(stderr, "warning: BitmapFont::init failed\n");
        }
    }

    emit_event("ready");

    // Stdin worker: blocks on getline, posts each line as an SDL_USEREVENT.
    // SDL_PushEvent is documented thread-safe; SDL_PollEvent / rendering is
    // not, so the main thread keeps those.
    std::atomic<bool> stop_stdin{false};
    std::thread stdin_thread([&stop_stdin]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (stop_stdin.load(std::memory_order_acquire)) return;
            auto* heap_line = new std::string(std::move(line));
            SDL_Event ev{};
            ev.type       = SDL_USEREVENT;
            ev.user.code  = static_cast<Sint32>(SDL_USEREVENT_JSON_LINE);
            ev.user.data1 = heap_line;
            if (SDL_PushEvent(&ev) <= 0) {
                // Queue full or SDL gone; drop line to avoid leaking
                // when we can't deliver.
                delete heap_line;
            }
        }
        // EOF — ask the main loop to wind down.
        SDL_Event quit_ev{};
        quit_ev.type = SDL_QUIT;
        SDL_PushEvent(&quit_ev);
    });

    // Main loop. Even with no window we still pump SDL so the worker's
    // SDL_PushEvent calls get drained (SDL events still flow through the
    // main queue when only AUDIO is initialized).
    bool running = true;
    while (running) {
        if (g_signal_received.load() || state.should_quit) break;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
                break;
            }
            if (ev.type == SDL_USEREVENT &&
                ev.user.code == static_cast<Sint32>(SDL_USEREVENT_JSON_LINE)) {
                std::unique_ptr<std::string> line(
                    static_cast<std::string*>(ev.user.data1));
                handle_command_line(state, *line);
                if (state.should_quit) {
                    running = false;
                    break;
                }
                continue;
            }

            if (!ren) continue;

            if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                int w = 0, h = 0;
                SDL_GetRendererOutputSize(ren, &w, &h);
                ToolbarLayout L = compute_layout(w, h);
                int mx = ev.button.x, my = ev.button.y;

                if (point_in_rect(mx, my, L.play_btn)) {
                    if (state.playing) {
                        stop_audio(state);
                        emit_event("stopped");
                    } else if (state.has_program) {
                        if (start_audio(state)) emit_event("started");
                        else emit_error("failed to start audio");
                    }
                } else if (point_in_rect(mx, my, L.mode_wave_half)) {
                    state.display_mode = DisplayMode::Waveform;
                } else if (point_in_rect(mx, my, L.mode_osc_half)) {
                    state.display_mode = DisplayMode::Oscilloscope;
                } else {
                    // Slider area: clickable along the whole toolbar strip
                    // between the mode toggle and the readout.
                    SDL_Rect slider_hit{L.slider_track.x, L.toolbar.y,
                                        L.slider_track.w, L.toolbar.h};
                    if (point_in_rect(mx, my, slider_hit)) {
                        state.slider_dragging = true;
                        state.master_volume = volume_from_x(mx, L);
                        state.engine->set_master_volume(state.master_volume);
                    }
                }
            } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
                state.slider_dragging = false;
            } else if (ev.type == SDL_MOUSEMOTION && state.slider_dragging) {
                int w = 0, h = 0;
                SDL_GetRendererOutputSize(ren, &w, &h);
                ToolbarLayout L = compute_layout(w, h);
                state.master_volume = volume_from_x(ev.motion.x, L);
                state.engine->set_master_volume(state.master_volume);
            }
        }
        if (!running) break;

        if (ren) {
            RenderInputs in{
                state.engine,
                state.playing,
                state.has_program,
                state.display_mode,
                state.master_volume,
                state.engine->vm().context().bpm,
                &state.wave_view,
                &state.osc_view,
            };
            render_frame(ren, font_ok ? &font : nullptr, in);
            SDL_RenderPresent(ren);
        } else {
            // No window — sleep a bit so we don't spin the CPU. The worker
            // is what wakes us via SDL_PushEvent.
            SDL_Delay(16);
        }
    }

    // Tear down stdin worker. std::cin is still blocked inside getline; the
    // simplest clean shutdown is to close stdin so getline returns. We can't
    // really do that portably from here, so detach: the process is exiting
    // anyway.
    stop_stdin.store(true, std::memory_order_release);
    stdin_thread.detach();

    if (font_ok) font.shutdown();
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    if (state.playing) {
        state.engine->stop();
    }
    return EXIT_SUCCESS;
}

}  // namespace nkido
