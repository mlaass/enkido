// nkido-catalog — CLI for the sample catalog library (scan / stats /
// gen-corpus). Testing and headless use; the studio embeds the library.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "catalog/corpus.hpp"
#include "catalog/database.hpp"
#include "catalog/scanner.hpp"

namespace {

void print_usage() {
    std::printf(
        "nkido-catalog — local sample catalog\n"
        "\n"
        "Usage:\n"
        "  nkido-catalog scan <folder> [--db <file>]   index a folder (registers it as a root)\n"
        "  nkido-catalog rescan [--db <file>]          re-scan every registered root\n"
        "  nkido-catalog stats [--db <file>]           show catalog statistics\n"
        "  nkido-catalog gen-corpus <count> <folder>   write a synthetic WAV test corpus\n"
        "\n"
        "The default database is $XDG_DATA_HOME/nkido/catalog.db (or\n"
        "~/.local/share/nkido/catalog.db); override with --db or NKIDO_CATALOG_DB.\n");
}

std::string default_db_path() {
    if (const char* env = std::getenv("NKIDO_CATALOG_DB"); env && *env) return env;
    std::filesystem::path base;
#if defined(_WIN32)
    if (const char* appdata = std::getenv("LOCALAPPDATA"); appdata && *appdata) base = appdata;
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home) {
        base = std::filesystem::path(home) / "Library" / "Application Support";
    }
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        base = xdg;
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        base = std::filesystem::path(home) / ".local" / "share";
    }
#endif
    if (base.empty()) base = std::filesystem::temp_directory_path();
    return (base / "nkido" / "catalog.db").string();
}

int fail(const std::string& msg) {
    std::fprintf(stderr, "error: %s\n", msg.c_str());
    return 1;
}

void print_scan_stats(const catalog::ScanStats& s) {
    std::printf("seen %llu | added %llu | updated %llu | unchanged %llu | errors %llu%s\n",
                static_cast<unsigned long long>(s.seen), static_cast<unsigned long long>(s.added),
                static_cast<unsigned long long>(s.updated),
                static_cast<unsigned long long>(s.unchanged),
                static_cast<unsigned long long>(s.errors), s.cancelled ? " | CANCELLED" : "");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    const std::string mode = argv[1];

    std::string db_path = default_db_path();
    std::string positional[2];
    int npos = 0;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (npos < 2) {
            positional[npos++] = argv[i];
        }
    }

    if (mode == "gen-corpus") {
        if (npos != 2) return fail("gen-corpus needs <count> and <folder>");
        const auto count = std::strtoull(positional[0].c_str(), nullptr, 10);
        if (count == 0) return fail("count must be a positive integer");
        const auto written = catalog::generate_corpus(positional[1], count);
        std::printf("wrote %llu files to %s\n", static_cast<unsigned long long>(written),
                    positional[1].c_str());
        return written == count ? 0 : 1;
    }

    std::string error;
    auto db = catalog::Database::open(db_path, &error);
    if (!db) return fail(error);
    if (db->was_rebuilt()) {
        std::fprintf(stderr, "warning: catalog was corrupt and has been rebuilt (old file kept "
                             "beside it); rescan to repopulate\n");
    }

    if (mode == "scan") {
        if (npos != 1) return fail("scan needs a <folder>");
        const auto root_id = db->add_root(positional[0], "user", &error);
        if (root_id < 0) return fail(error);
        catalog::ScanOptions options;
        options.progress = [](const catalog::ScanStats& s) {
            std::fprintf(stderr, "\r%llu files...", static_cast<unsigned long long>(s.seen));
            return true;
        };
        const auto stats = catalog::Scanner(*db).scan_root(root_id, options);
        std::fprintf(stderr, "\r");
        if (stats.root_missing) return fail("root folder does not exist");
        print_scan_stats(stats);
        return 0;
    }

    if (mode == "rescan") {
        const auto stats = catalog::Scanner(*db).scan_all();
        print_scan_stats(stats);
        return 0;
    }

    if (mode == "stats") {
        const auto s = db->stats();
        std::printf("database: %s (schema v%d)\n", db->path().c_str(), db->schema_version());
        std::printf("roots: %lld\n", static_cast<long long>(s.roots));
        for (const auto& r : db->roots()) {
            std::printf("  [%lld] %s (%s)%s\n", static_cast<long long>(r.id), r.path.c_str(),
                        r.kind.c_str(), r.enabled ? "" : " [disabled]");
        }
        std::printf("files: %lld (%lld missing, %lld decode errors)\n",
                    static_cast<long long>(s.files), static_cast<long long>(s.missing),
                    static_cast<long long>(s.decode_errors));
        std::printf("size: %.1f MB, duration: %.1f min\n",
                    static_cast<double>(s.total_bytes) / (1024.0 * 1024.0),
                    s.total_duration_s / 60.0);
        return 0;
    }

    print_usage();
    return 1;
}
