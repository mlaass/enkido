#pragma once

#include "akkado/codegen.hpp"   // for RequiredSample, UriRequest
#include "cedar/io/file_cache.hpp"
#include "cedar/vm/vm.hpp"

#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nkido {

/// Strudel-manifest entry: one key (sample name) maps to one or more
/// relative paths (variants). A `_base` field may set the base URL; if
/// absent we derive one from the manifest URI.
struct BankManifest {
    std::string base_url;                                          // absolute base URL for variant paths
    std::unordered_map<std::string, std::vector<std::string>> samples;
};

/// Set up the process-wide UriResolver with the standard native handler
/// stack: file://, http://, https:// (cached via the supplied FileCache),
/// github:, bundled://. Idempotent — calling twice replaces the handlers.
void register_native_handlers(cedar::FileCache& cache);

/// Fetch and parse a strudel.json manifest from any URI scheme. Throws
/// `std::runtime_error` on fetch / parse failure. The returned manifest's
/// `base_url` is guaranteed absolute (HTTP/HTTPS URL or file:// path).
BankManifest fetch_bank_manifest(const std::string& uri);

/// Fetch a SoundFont (.sf2/.sf3) from any URI and load it into
/// `vm.soundfont_registry()`. `display_name` is recorded for the registry
/// entry. Returns the assigned SoundFont ID, or -1 on failure.
int load_soundfont_uri(cedar::VM& vm, const std::string& uri, const std::string& display_name);

/// Fetch a single audio file (WAV/OGG/FLAC/MP3) and register it in
/// `vm.sample_bank()` under `name`. Returns the sample ID (>=1) on success
/// or 0 on failure.
std::uint32_t load_sample_uri(cedar::VM& vm, const std::string& uri, const std::string& name);

/// Fetch every variant referenced by `required` from the supplied bank
/// manifests and register each into `vm.sample_bank()` under its qualified
/// name (matching `RequiredSample::qualified_name()`). The `default_banks`
/// list is consulted in order when a RequiredSample's bank field is empty
/// or "default"; named banks take their entry from `named_banks`. Missing
/// samples are reported on stderr but do not abort the run.
///
/// Returns the number of samples successfully registered.
std::size_t register_required_samples(
    cedar::VM& vm,
    const std::vector<akkado::RequiredSample>& required,
    const std::vector<BankManifest>& default_banks,
    const std::unordered_map<std::string, BankManifest>& named_banks);

/// Discover the built-in default sample-kit manifest URI. Tries (in order):
///   1. `NKIDO_DEFAULT_KIT` env var (URI or bare path → file://).
///      An empty value is treated as an explicit silent opt-out.
///   2. `NKIDO_DEFAULT_KIT_PATH` compile-time macro (in-tree builds).
///   3. Walk-up from the current working directory looking for
///      `web/static/samples/bpb_808_clean/strudel.json`.
///   4. Install-relative `<binary_dir>/../share/nkido/default_kit/strudel.json`.
///
/// Returns `std::nullopt` if nothing is found (after which the caller should
/// emit a one-line info diagnostic and continue without a default kit).
std::optional<std::string> find_default_bank_uri(std::ostream& diag);

/// Resolve a short SoundFont alias (e.g. "gm", "gm_medium", "gm_large") to a
/// file:// URI pointing at a bundled .sf2/.sf3 on disk. Mirrors the web app's
/// DEFAULT_SOUNDFONTS table in `web/src/lib/audio/default-soundfonts.ts`.
///
/// Search order for the underlying file:
///   1. `$NKIDO_SOUNDFONT_DIR` env var (directory of .sf2/.sf3 files).
///   2. `NKIDO_SOUNDFONT_PATH` compile-time macro (in-tree builds).
///   3. Walk-up from CWD looking for `web/static/soundfonts/<file>`.
///   4. `$HOME/.nkido/soundfonts/<file>`.
///   5. Install-relative `<binary_dir>/../share/nkido/soundfonts/<file>`.
///
/// Returns `std::nullopt` for unknown aliases or when no bundled file is
/// findable. Callers should fall through to treating the name as a literal
/// URI (so `soundfont(@, "https://...", N)` and `soundfont(@, "piano.sf2", N)`
/// keep working).
std::optional<std::string> resolve_soundfont_alias(const std::string& alias);

/// Resolve a SoundFont alias, consulting runtime `--soundfont-alias` entries
/// first. The runtime alias chain is followed depth-limited (cycle guard);
/// once it terminates, a built-in alias name resolves to its bundled file and
/// any other value is returned as a URI (bare paths become `file://`). Returns
/// `std::nullopt` when `alias` is neither a runtime nor a built-in alias, so
/// callers fall through to treating it as a literal URI/path.
std::optional<std::string> resolve_soundfont_alias(
    const std::string& alias,
    const std::map<std::string, std::string>& runtime_aliases);

}  // namespace nkido
