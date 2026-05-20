#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <string>
#include <vector>
#include <span>
#include "diagnostics.hpp"
#include "codegen.hpp"  // For StateInitData
#include <cedar/vm/program_slot.hpp>  // For cedar::BlockEntry (FOREACH_EVENT table)
#include "symbol_table.hpp"  // For CompileResult::symbols (Phase 2 records-system-unification)
#include "ast.hpp"           // For CompileResult::ast (shared_ptr<Ast>)

namespace akkado {

// Forward declarations
class SampleRegistry;
class FileResolver;

/// Akkado version information (injected via CMake compile definitions)
#ifndef AKKADO_VERSION_MAJOR
#define AKKADO_VERSION_MAJOR 0
#define AKKADO_VERSION_MINOR 0
#define AKKADO_VERSION_PATCH 0
#endif

#define AKKADO_STRINGIFY_(x) #x
#define AKKADO_STRINGIFY(x) AKKADO_STRINGIFY_(x)

struct Version {
    static constexpr int major = AKKADO_VERSION_MAJOR;
    static constexpr int minor = AKKADO_VERSION_MINOR;
    static constexpr int patch = AKKADO_VERSION_PATCH;

    static constexpr std::string_view string() {
        return AKKADO_STRINGIFY(AKKADO_VERSION_MAJOR) "."
               AKKADO_STRINGIFY(AKKADO_VERSION_MINOR) "."
               AKKADO_STRINGIFY(AKKADO_VERSION_PATCH);
    }
};

// RequiredSample is defined in codegen.hpp

/// Compilation result
struct CompileResult {
    bool success = false;
    // bytecode holds the full [ main program | FOREACH_EVENT block bodies ]
    // instruction stream. main_instruction_count is the main/body boundary;
    // block_table locates each block body (PRD prd-runtime-functions-control-flow L3).
    // For programs with no FOREACH_EVENT blocks, block_table is empty and
    // main_instruction_count equals the total instruction count.
    std::vector<std::uint8_t> bytecode;
    std::uint32_t main_instruction_count = 0;
    std::vector<cedar::BlockEntry> block_table;
    std::vector<SourceLocation> source_locations;  // Parallel to bytecode instructions, tracks origin
    std::vector<Diagnostic> diagnostics;
    std::vector<StateInitData> state_inits;  // State initialization data for patterns
    std::vector<std::string> required_samples;  // Sample names used (for runtime loading) - legacy
    std::vector<RequiredSample> required_samples_extended;  // Sample refs with bank/variant info
    std::vector<ScalarSampleMapping> scalar_sample_mappings;  // Direct sample("name") calls needing runtime ID patching
    std::vector<RequiredSoundFont> required_soundfonts;  // SoundFont files needed at runtime
    // Per-call midi() source configs (PRD prd-midi-input §4.7). Each entry tells
    // the host to call vm.init_midi_queue_state(state_id, kind, name_or_path,
    // channel_filter, loop, tempo_mode) before resuming the audio thread, plus
    // (for File kind) load the .mid bytes via cedar_load_midi_file / UriResolver.
    std::vector<RequiredMidiSource> required_midi_sources;
    // Per-call midi_cc() routes (PRD prd-midi-input §4.8). Compile-time directives
    // that the host MIDI callback evaluates to call vm.set_param(name, value, slew).
    std::vector<RequiredMidiCcRoute> required_midi_cc_routes;
    // Source strings collected from in('...') calls in compile order (one entry per call,
    // empty string if the call had no argument). Hosts use this to switch input source.
    std::vector<std::string> required_input_sources;
    std::vector<ParamDecl> param_decls;  // Declared parameters for UI generation
    std::vector<VisualizationDecl> viz_decls;  // Declared visualizations for UI generation
    std::vector<BuiltinVarOverride> builtin_var_overrides;  // Builtin variable overrides (bpm, sr)
    // Wavetable banks declared via wt_load(). v1 keeps the *last* loaded bank
    // active; multi-bank routing is a v2 follow-up.
    std::vector<RequiredWavetable> required_wavetables;
    // URIs declared via top-level directives like samples("..."). Hosts iterate
    // these in source order, dispatch each by `kind` to the appropriate
    // registry, and block bytecode swap until every URI resolves.
    std::vector<UriRequest> required_uris;

    // Phase 2 records-system-unification: analyzer outputs retained for
    // downstream tooling (e.g. shape index serialization). Populated by
    // compile() whenever the corresponding pass runs, including on early-
    // return paths so partial state can still be inspected. Not consumed by
    // the audio runtime; safe to ignore for normal compile-and-play.
    std::optional<SymbolTable> symbols;
    std::shared_ptr<Ast>       ast;
};

/// Compile Akkado source code to Cedar bytecode
/// @param source The source code to compile
/// @param filename Optional filename for error reporting
/// @param sample_registry Optional sample registry for resolving sample names to IDs
/// @param resolver Optional file resolver for import statements
/// @param lint_strict Enable opt-in lint warnings (e.g. W201 dotted hole-field).
/// @return Compilation result with bytecode and diagnostics
CompileResult compile(std::string_view source, std::string_view filename = "<input>",
                     SampleRegistry* sample_registry = nullptr,
                     const FileResolver* resolver = nullptr,
                     bool lint_strict = false);

/// Compile from file (creates a FilesystemResolver for the file's directory)
/// @param path Path to the source file
/// @param sample_registry Optional sample registry for resolving sample names to IDs
/// @param resolver Optional file resolver (if null, creates a FilesystemResolver)
/// @param lint_strict Enable opt-in lint warnings (e.g. W201 dotted hole-field).
/// @return Compilation result
CompileResult compile_file(const std::string& path,
                          SampleRegistry* sample_registry = nullptr,
                          const FileResolver* resolver = nullptr,
                          bool lint_strict = false);

} // namespace akkado
