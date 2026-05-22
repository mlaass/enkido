#include "asset_loader.hpp"

#include "cedar/io/handlers/bundled_handler.hpp"
#include "cedar/io/handlers/file_handler.hpp"
#include "cedar/io/handlers/github_handler.hpp"
#include "cedar/io/handlers/http_handler.hpp"
#include "cedar/io/uri_resolver.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace nkido {

namespace {

/// Minimal recursive-descent parser for the strudel.json subset:
/// `{ "<key>": <string-or-array-of-strings>, ... }`. No numbers, booleans,
/// nulls, or nested objects — those keys are silently skipped. Robust
/// enough for every published Dirt-Samples / TR808 / TR909 manifest.
class ManifestScanner {
public:
    explicit ManifestScanner(std::string_view text) : src_(text) {}

    BankManifest parse() {
        BankManifest m;
        skip_ws();
        if (!consume('{')) throw std::runtime_error("manifest: expected '{'");
        skip_ws();
        if (peek() == '}') { advance(); return m; }

        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            if (!consume(':')) throw std::runtime_error("manifest: expected ':' after key '" + key + "'");
            skip_ws();
            char c = peek();
            if (c == '"') {
                std::string value = parse_string();
                if (key == "_base") {
                    m.base_url = std::move(value);
                } else if (key.empty() || key[0] == '_') {
                    // ignore _name and other underscore-prefixed metadata
                } else {
                    m.samples[std::move(key)] = {std::move(value)};
                }
            } else if (c == '[') {
                auto values = parse_string_array();
                if (key.empty() || key[0] == '_') {
                    // ignore underscore-prefixed array fields
                } else {
                    m.samples[std::move(key)] = std::move(values);
                }
            } else {
                // Skip unsupported value types (numbers, booleans, null, objects)
                skip_value();
            }
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            throw std::runtime_error("manifest: expected ',' or '}'");
        }
        return m;
    }

private:
    char peek() {
        return pos_ < src_.size() ? src_[pos_] : '\0';
    }
    char advance() {
        return pos_ < src_.size() ? src_[pos_++] : '\0';
    }
    bool consume(char c) {
        if (peek() == c) { advance(); return true; }
        return false;
    }
    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    std::string parse_string() {
        if (!consume('"')) throw std::runtime_error("manifest: expected string literal");
        std::string out;
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (pos_ >= src_.size()) break;
                char esc = src_[pos_++];
                switch (esc) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    default:   out.push_back(esc); break;  // permissive
                }
            } else {
                out.push_back(c);
            }
        }
        throw std::runtime_error("manifest: unterminated string");
    }

    std::vector<std::string> parse_string_array() {
        std::vector<std::string> out;
        if (!consume('[')) throw std::runtime_error("manifest: expected '['");
        skip_ws();
        if (consume(']')) return out;
        while (true) {
            skip_ws();
            out.push_back(parse_string());
            skip_ws();
            if (consume(',')) continue;
            if (consume(']')) break;
            throw std::runtime_error("manifest: expected ',' or ']' in array");
        }
        return out;
    }

    void skip_value() {
        // Skip a JSON value that we don't support (numbers, booleans, null,
        // nested objects). Greedy: read until comma/brace at the same nesting
        // level, balancing quotes and brackets.
        int depth = 0;
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == '"') {
                parse_string();
                continue;
            }
            if (c == '{' || c == '[') { ++depth; ++pos_; continue; }
            if (c == '}' || c == ']') {
                if (depth == 0) return;
                --depth; ++pos_; continue;
            }
            if (c == ',' && depth == 0) return;
            ++pos_;
        }
    }

    std::string_view src_;
    std::size_t pos_ = 0;
};

/// Minimal recursive-descent parser for the combined Tidal Drum Machines
/// catalog: a top-level object with `_base`/`_commit` strings and a `banks`
/// object whose entries are themselves objects (`_path` string + sound-code
/// → string-array). Reuses the same JSON subset as ManifestScanner; nested
/// objects are handled explicitly here (ManifestScanner skips them).
class TdmCatalogScanner {
public:
    explicit TdmCatalogScanner(std::string_view text) : src_(text) {}

    TdmCatalog parse() {
        TdmCatalog cat;
        skip_ws();
        if (!consume('{')) throw std::runtime_error("catalog: expected '{'");
        skip_ws();
        if (consume('}')) return cat;

        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            if (!consume(':')) throw std::runtime_error("catalog: expected ':' after '" + key + "'");
            skip_ws();
            if (key == "_base" && peek() == '"') {
                cat.base = parse_string();
            } else if (key == "_commit" && peek() == '"') {
                cat.commit = parse_string();
            } else if (key == "banks" && peek() == '{') {
                parse_banks(cat);
            } else {
                skip_value();
            }
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            throw std::runtime_error("catalog: expected ',' or '}'");
        }
        if (cat.base.empty()) throw std::runtime_error("catalog: missing _base");
        return cat;
    }

private:
    void parse_banks(TdmCatalog& cat) {
        if (!consume('{')) throw std::runtime_error("catalog: expected '{' for banks");
        skip_ws();
        if (consume('}')) return;
        while (true) {
            skip_ws();
            std::string bank_name = parse_string();
            skip_ws();
            if (!consume(':')) throw std::runtime_error("catalog: expected ':' after bank name");
            skip_ws();
            cat.banks.emplace(std::move(bank_name), parse_bank());
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            throw std::runtime_error("catalog: expected ',' or '}' in banks");
        }
    }

    TdmCatalogBank parse_bank() {
        TdmCatalogBank bank;
        if (!consume('{')) throw std::runtime_error("catalog: expected '{' for bank");
        skip_ws();
        if (consume('}')) return bank;
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            if (!consume(':')) throw std::runtime_error("catalog: expected ':' in bank");
            skip_ws();
            if (key == "_path" && peek() == '"') {
                bank.path = parse_string();
            } else if (peek() == '[') {
                bank.sounds[std::move(key)] = parse_string_array();
            } else {
                skip_value();
            }
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) break;
            throw std::runtime_error("catalog: expected ',' or '}' in bank");
        }
        return bank;
    }

    char peek() { return pos_ < src_.size() ? src_[pos_] : '\0'; }
    char advance() { return pos_ < src_.size() ? src_[pos_++] : '\0'; }
    bool consume(char c) { if (peek() == c) { advance(); return true; } return false; }
    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }
    std::string parse_string() {
        if (!consume('"')) throw std::runtime_error("catalog: expected string");
        std::string out;
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (pos_ >= src_.size()) break;
                char esc = src_[pos_++];
                switch (esc) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    default:  out.push_back(esc); break;  // ", \, /, permissive
                }
            } else {
                out.push_back(c);
            }
        }
        throw std::runtime_error("catalog: unterminated string");
    }
    std::vector<std::string> parse_string_array() {
        std::vector<std::string> out;
        if (!consume('[')) throw std::runtime_error("catalog: expected '['");
        skip_ws();
        if (consume(']')) return out;
        while (true) {
            skip_ws();
            out.push_back(parse_string());
            skip_ws();
            if (consume(',')) continue;
            if (consume(']')) break;
            throw std::runtime_error("catalog: expected ',' or ']' in array");
        }
        return out;
    }
    void skip_value() {
        int depth = 0;
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == '"') { parse_string(); continue; }
            if (c == '{' || c == '[') { ++depth; ++pos_; continue; }
            if (c == '}' || c == ']') {
                if (depth == 0) return;
                --depth; ++pos_; continue;
            }
            if (c == ',' && depth == 0) return;
            ++pos_;
        }
    }

    std::string_view src_;
    std::size_t pos_ = 0;
};

/// Percent-encode a relative URL path so filenames with spaces or other
/// reserved characters resolve correctly against raw.githubusercontent.com.
/// Path separators and the RFC 3986 unreserved set pass through unchanged.
std::string url_encode_path(const std::string& path) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (char raw : path) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string derive_base_url(const std::string& uri) {
    // Strip everything after the last '/' to get the directory URL.
    auto last = uri.find_last_of('/');
    if (last == std::string::npos) return {};
    return uri.substr(0, last + 1);
}

std::string github_to_https(const std::string& uri) {
    if (uri.rfind("github:", 0) != 0) return uri;
    return cedar::GithubHandler::to_https_url(uri);
}

}  // namespace

void register_native_handlers(cedar::FileCache& cache) {
    auto& r = cedar::UriResolver::instance();
    r.register_handler(std::make_unique<cedar::FileHandler>());
    r.register_handler(std::make_unique<cedar::HttpHandler>("http", &cache));
    r.register_handler(std::make_unique<cedar::HttpHandler>("https", &cache));
    r.register_handler(std::make_unique<cedar::GithubHandler>());
    r.register_handler(std::make_unique<cedar::BundledHandler>());
}

BankManifest fetch_bank_manifest(const std::string& uri) {
    auto& r = cedar::UriResolver::instance();
    auto result = r.load(uri);
    if (!result.success()) {
        throw std::runtime_error("fetch failed: " + result.error().message);
    }

    const auto& bytes = result.buffer();
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    ManifestScanner scanner(text);
    BankManifest m = scanner.parse();

    if (m.base_url.empty()) {
        // Derive from the resolved URI. github: maps to https first.
        std::string base_source = github_to_https(uri);
        m.base_url = derive_base_url(base_source);
    }
    if (!m.base_url.empty() && m.base_url.back() != '/') m.base_url.push_back('/');
    return m;
}

TdmCatalog parse_tdm_catalog(std::string_view json) {
    TdmCatalogScanner scanner(json);
    TdmCatalog cat = scanner.parse();
    if (cat.base.empty()) {
        throw std::runtime_error("catalog: missing _base");
    }
    if (cat.base.back() != '/') cat.base.push_back('/');
    return cat;
}

int load_soundfont_uri(cedar::VM& vm, const std::string& uri, const std::string& display_name) {
#ifndef CEDAR_NO_SOUNDFONT
    auto& r = cedar::UriResolver::instance();
    auto result = r.load(uri);
    if (!result.success()) {
        std::cerr << "error: SoundFont fetch '" << uri << "' failed: "
                  << result.error().message << "\n";
        return -1;
    }
    const auto& bytes = result.buffer();
    return vm.soundfont_registry().load_from_memory(
        cedar::MemoryView(bytes.data(), bytes.size()), display_name, vm.sample_bank());
#else
    (void)vm; (void)uri; (void)display_name;
    std::cerr << "error: nkido-cli built without SoundFont support\n";
    return -1;
#endif
}

std::uint32_t load_sample_uri(cedar::VM& vm, const std::string& uri, const std::string& name) {
    auto& r = cedar::UriResolver::instance();
    auto result = r.load(uri);
    if (!result.success()) {
        std::cerr << "error: sample fetch '" << uri << "' failed: "
                  << result.error().message << "\n";
        return 0;
    }
    const auto& bytes = result.buffer();
    return vm.sample_bank().load_audio_data(name, cedar::MemoryView(bytes.data(), bytes.size()));
}

namespace {

const std::string* lookup_variant(const BankManifest& m,
                                  const std::string& sample_name,
                                  int variant) {
    auto it = m.samples.find(sample_name);
    if (it == m.samples.end() || it->second.empty()) return nullptr;
    int actual = variant % static_cast<int>(it->second.size());
    if (actual < 0) actual += static_cast<int>(it->second.size());
    return &it->second[static_cast<std::size_t>(actual)];
}

std::string resolve_sample_url(const BankManifest& m, const std::string& path) {
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0 ||
        path.rfind("file://", 0) == 0 || (!path.empty() && path[0] == '/')) {
        return path;
    }
    return m.base_url + path;
}

/// If `rel` (a `<Machine>/<sub>/<file>` path) exists under the local
/// download cache, return its `file://` URI. The cache is populated by
/// `scripts/download-tdm.sh` and lets the CLI play catalog kits offline.
std::optional<std::string> tdm_local_cache_path(const std::string& rel) {
    namespace fs = std::filesystem;
    fs::path cache_dir;
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && xdg[0]) {
        cache_dir = fs::path(xdg);
    } else if (const char* home = std::getenv("HOME"); home && home[0]) {
        cache_dir = fs::path(home) / ".cache";
    } else {
        return std::nullopt;
    }
    fs::path full = cache_dir / "nkido" / "tidal-drum-machines" / rel;
    std::error_code ec;
    if (!fs::is_regular_file(full, ec) || ec) return std::nullopt;
    fs::path canon = fs::canonical(full, ec);
    if (ec) canon = full;
    std::string s = canon.generic_string();
    if (!s.empty() && s.front() != '/') s.insert(s.begin(), '/');
    return "file://" + s;
}

/// Resolve a Tidal Drum Machines catalog sample to a URI. Prefers a local
/// download-cache copy (populated by scripts/download-tdm.sh) so the CLI
/// works offline; otherwise builds the pinned GitHub raw URL with the path
/// percent-encoded (filenames upstream contain spaces).
std::string resolve_tdm_sample_uri(const TdmCatalog& catalog,
                                   const TdmCatalogBank& bank,
                                   const std::string& rel) {
    if (auto local = tdm_local_cache_path(bank.path + rel)) {
        return *local;
    }
    return catalog.base + url_encode_path(bank.path + rel);
}

}  // namespace

std::size_t register_required_samples(
    cedar::VM& vm,
    const std::vector<akkado::RequiredSample>& required,
    const std::vector<BankManifest>& default_banks,
    const std::unordered_map<std::string, BankManifest>& named_banks,
    const TdmCatalog* catalog) {
    std::size_t loaded = 0;

    for (const auto& req : required) {
        if (vm.sample_bank().has_sample(req.qualified_name())) {
            ++loaded;
            continue;
        }

        // Tidal Drum Machines catalog banks resolve ahead of user banks: a
        // `.bank("RolandTR808")` whose name matches the built-in catalog
        // streams the pinned GitHub raw URL (cached via the https handler).
        // A missing variant or a failed fetch is a soft failure — warn and
        // continue so the rest of the program keeps playing.
        if (catalog && !req.bank.empty() && req.bank != "default" &&
            catalog->has_bank(req.bank)) {
            const TdmCatalogBank& bank = catalog->banks.at(req.bank);
            auto sit = bank.sounds.find(req.name);
            if (sit == bank.sounds.end() || sit->second.empty()) {
                std::cerr << "warning: sample '" << req.qualified_name()
                          << "' not found in catalog bank '" << req.bank
                          << "'\n";
                continue;
            }
            const int n = static_cast<int>(sit->second.size());
            int idx = req.variant % n;
            if (idx < 0) idx += n;
            const std::string& rel = sit->second[static_cast<std::size_t>(idx)];
            std::string url = resolve_tdm_sample_uri(*catalog, bank, rel);
            if (load_sample_uri(vm, url, req.qualified_name()) != 0) {
                ++loaded;
            }
            continue;
        }

        // Search the appropriate bank list.
        const std::string* path = nullptr;
        const BankManifest* hit_bank = nullptr;

        if (req.bank.empty() || req.bank == "default") {
            for (const auto& m : default_banks) {
                path = lookup_variant(m, req.name, req.variant);
                if (path) { hit_bank = &m; break; }
            }
        } else {
            auto it = named_banks.find(req.bank);
            if (it != named_banks.end()) {
                path = lookup_variant(it->second, req.name, req.variant);
                if (path) hit_bank = &it->second;
            }
        }

        if (!path || !hit_bank) {
            std::cerr << "warning: sample '" << req.qualified_name()
                      << "' not found in any loaded bank\n";
            continue;
        }

        std::string url = resolve_sample_url(*hit_bank, *path);
        if (load_sample_uri(vm, url, req.qualified_name()) != 0) {
            ++loaded;
        }
    }

    return loaded;
}

namespace {

constexpr const char* kDefaultKitRelative =
    "web/static/samples/bpb_808_clean/strudel.json";

constexpr const char* kTdmCatalogRelative =
    "web/static/samples/tidal-drum-machines/catalog.json";

// Convert a filesystem path to a file:// URI. Already-URI inputs (with a
// "scheme://" prefix) pass through unchanged.
std::string path_to_uri(const std::string& path) {
    if (path.find("://") != std::string::npos) return path;
    if (path.empty()) return path;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path absolute = fs::absolute(fs::path(path), ec);
    if (ec) absolute = fs::path(path);
    std::string s = absolute.generic_string();
    if (!s.empty() && s.front() != '/') s.insert(s.begin(), '/');
    return "file://" + s;
}

bool path_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec) && !ec;
}

// Resolve the path of the running executable. Used for the install-relative
// fallback (`<binary_dir>/../share/nkido/default_kit/strudel.json`).
std::optional<std::filesystem::path> executable_dir() {
#if defined(__linux__)
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::nullopt;
    buf[n] = '\0';
    return std::filesystem::path(buf).parent_path();
#elif defined(__APPLE__)
    char buf[4096];
    std::uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return std::nullopt;
    std::error_code ec;
    auto canon = std::filesystem::canonical(std::filesystem::path(buf), ec);
    if (ec) canon = std::filesystem::path(buf);
    return canon.parent_path();
#elif defined(_WIN32)
    char buf[4096];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (n == 0) return std::nullopt;
    return std::filesystem::path(std::string(buf, n)).parent_path();
#else
    return std::nullopt;
#endif
}

}  // namespace

std::optional<std::string> resolve_soundfont_alias(const std::string& alias) {
    namespace fs = std::filesystem;

    // Mirrors web/src/lib/audio/default-soundfonts.ts. The first existing
    // candidate per alias wins; .sf3 (compressed) preferred over .sf2 (PCM)
    // because the bundled set is .sf3 today.
    struct AliasEntry {
        const char* name;
        const char* files[3];  // null-terminated, max two candidates per alias
    };
    static constexpr AliasEntry kAliases[] = {
        {"gm",        {"TimGM6mb.sf3",        "TimGM6mb.sf2",        nullptr}},
        {"gm_medium", {"FluidR3Mono_GM.sf3",  nullptr,               nullptr}},
        {"gm_large",  {"MuseScore_General.sf3", nullptr,             nullptr}},
    };

    const AliasEntry* entry = nullptr;
    for (const auto& e : kAliases) {
        if (alias == e.name) { entry = &e; break; }
    }
    if (!entry) return std::nullopt;

    // Build the candidate search directories in priority order.
    std::vector<fs::path> dirs;

    if (const char* env = std::getenv("NKIDO_SOUNDFONT_DIR")) {
        if (env[0] != '\0') dirs.emplace_back(env);
    }

#ifdef NKIDO_SOUNDFONT_PATH
    dirs.emplace_back(NKIDO_SOUNDFONT_PATH);
#endif

    // Walk up from CWD looking for web/static/soundfonts/.
    {
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);
        if (!ec) {
            for (fs::path dir = cwd; !dir.empty(); ) {
                fs::path candidate = dir / "web" / "static" / "soundfonts";
                if (fs::is_directory(candidate, ec) && !ec) {
                    dirs.push_back(std::move(candidate));
                    break;
                }
                fs::path parent = dir.parent_path();
                if (parent == dir) break;
                dir = std::move(parent);
            }
        }
    }

    if (const char* home = std::getenv("HOME")) {
        if (home[0] != '\0') {
            dirs.emplace_back(fs::path(home) / ".nkido" / "soundfonts");
        }
    }

    if (auto exe_dir = executable_dir()) {
        dirs.push_back(*exe_dir / ".." / "share" / "nkido" / "soundfonts");
    }

    for (const auto& dir : dirs) {
        for (int i = 0; i < 3 && entry->files[i]; ++i) {
            fs::path candidate = dir / entry->files[i];
            std::error_code ec;
            if (fs::exists(candidate, ec) && !ec) {
                fs::path canon = fs::canonical(candidate, ec);
                if (ec) canon = candidate;
                return path_to_uri(canon.generic_string());
            }
        }
    }

    return std::nullopt;
}

std::optional<std::string> resolve_soundfont_alias(
    const std::string& alias,
    const std::map<std::string, std::string>& runtime_aliases) {
    // Follow the runtime alias chain (depth-limited cycle guard).
    std::string cur = alias;
    for (int depth = 0; depth < 8; ++depth) {
        auto it = runtime_aliases.find(cur);
        if (it == runtime_aliases.end()) break;
        cur = it->second;
    }

    // A built-in alias name resolves to its bundled file.
    if (auto bundled = resolve_soundfont_alias(cur)) {
        return bundled;
    }

    // If at least one runtime alias was followed, `cur` is a user-supplied
    // path/URI — hand it back so the URI resolver loads it directly.
    if (cur != alias) {
        if (cur.find("://") != std::string::npos) return cur;
        return path_to_uri(cur);
    }

    // Neither a runtime nor a built-in alias — caller treats it literally.
    return std::nullopt;
}

std::optional<std::string> find_default_bank_uri(std::ostream& diag) {
    namespace fs = std::filesystem;

    // 1. Env var override. Empty = explicit silent opt-out.
    if (const char* env = std::getenv("NKIDO_DEFAULT_KIT")) {
        std::string value(env);
        if (value.empty()) return std::nullopt;
        if (value.find("://") == std::string::npos && !path_exists(value)) {
            diag << "info: NKIDO_DEFAULT_KIT='" << value
                 << "' does not exist on disk; will still attempt fetch\n";
        }
        return path_to_uri(value);
    }

    // 2. Compile-time macro (in-tree dev builds).
#ifdef NKIDO_DEFAULT_KIT_PATH
    {
        std::string p(NKIDO_DEFAULT_KIT_PATH);
        if (!p.empty() && path_exists(p)) {
            return path_to_uri(p);
        }
    }
#endif

    // 3. Walk up from CWD looking for web/static/samples/bpb_808_clean/strudel.json.
    {
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);
        if (!ec) {
            for (fs::path dir = cwd; !dir.empty(); ) {
                fs::path candidate = dir / kDefaultKitRelative;
                if (fs::exists(candidate, ec) && !ec) {
                    return path_to_uri(candidate.generic_string());
                }
                fs::path parent = dir.parent_path();
                if (parent == dir) break;
                dir = std::move(parent);
            }
        }
    }

    // 4. Install-relative fallback.
    if (auto exe_dir = executable_dir()) {
        fs::path candidate = *exe_dir / ".." / "share" / "nkido"
                             / "default_kit" / "strudel.json";
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec) {
            fs::path canon = fs::canonical(candidate, ec);
            if (ec) canon = candidate;
            return path_to_uri(canon.generic_string());
        }
    }

    return std::nullopt;
}

std::optional<std::string> find_tdm_catalog_uri(std::ostream& diag) {
    namespace fs = std::filesystem;

    // 1. Env var override. Empty = explicit silent opt-out.
    if (const char* env = std::getenv("NKIDO_TDM_CATALOG")) {
        std::string value(env);
        if (value.empty()) return std::nullopt;
        if (value.find("://") == std::string::npos && !path_exists(value)) {
            diag << "info: NKIDO_TDM_CATALOG='" << value
                 << "' does not exist on disk; will still attempt fetch\n";
        }
        return path_to_uri(value);
    }

    // 2. Compile-time macro (in-tree dev builds).
#ifdef NKIDO_TDM_CATALOG_PATH
    {
        std::string p(NKIDO_TDM_CATALOG_PATH);
        if (!p.empty() && path_exists(p)) {
            return path_to_uri(p);
        }
    }
#endif

    // 3. Walk up from CWD looking for the committed catalog.
    {
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);
        if (!ec) {
            for (fs::path dir = cwd; !dir.empty(); ) {
                fs::path candidate = dir / kTdmCatalogRelative;
                if (fs::exists(candidate, ec) && !ec) {
                    return path_to_uri(candidate.generic_string());
                }
                fs::path parent = dir.parent_path();
                if (parent == dir) break;
                dir = std::move(parent);
            }
        }
    }

    // 4. Install-relative fallback.
    if (auto exe_dir = executable_dir()) {
        fs::path candidate = *exe_dir / ".." / "share" / "nkido"
                             / "tidal-drum-machines" / "catalog.json";
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec) {
            fs::path canon = fs::canonical(candidate, ec);
            if (ec) canon = candidate;
            return path_to_uri(canon.generic_string());
        }
    }

    return std::nullopt;
}

std::optional<TdmCatalog> load_tdm_catalog(std::ostream& diag) {
    auto uri = find_tdm_catalog_uri(diag);
    if (!uri) {
        diag << "info: Tidal Drum Machines catalog not found; "
                ".bank(\"<Machine>\") falls back to unknown-bank behavior\n";
        return std::nullopt;
    }
    auto& r = cedar::UriResolver::instance();
    auto result = r.load(*uri);
    if (!result.success()) {
        diag << "info: drum-machine catalog '" << *uri << "' could not be "
                "loaded: " << result.error().message << "\n";
        return std::nullopt;
    }
    const auto& bytes = result.buffer();
    std::string_view text(reinterpret_cast<const char*>(bytes.data()),
                          bytes.size());
    try {
        return parse_tdm_catalog(text);
    } catch (const std::exception& e) {
        diag << "info: drum-machine catalog '" << *uri
             << "' failed to parse: " << e.what() << "\n";
        return std::nullopt;
    }
}

}  // namespace nkido
