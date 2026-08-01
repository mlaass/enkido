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
struct CompileContext;  // akkado/compile_context.hpp

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

/// Cedar bytecode + instruction-level metadata (prd-parser-codegen-hardening
/// Phase 2 grouping — everything the host loads into the VM).
struct CompiledProgram {
    // bytecode holds the full [ main program | FOREACH_EVENT block bodies ]
    // instruction stream. main_instruction_count is the main/body boundary;
    // block_table locates each block body (PRD prd-runtime-functions-control-flow L3).
    // For programs with no FOREACH_EVENT blocks, block_table is empty and
    // main_instruction_count equals the total instruction count.
    std::vector<std::uint8_t> bytecode;
    std::uint32_t main_instruction_count = 0;
    std::vector<cedar::BlockEntry> block_table;
    std::vector<SourceLocation> source_locations;  // Parallel to bytecode instructions, tracks origin
    std::vector<StateInitData> state_inits;  // State initialization data for patterns

    // Peak distinct buffer indices the compiled program references. Hosts
    // call `vm.buffers().ensure_capacity(required_buffers)` off the audio
    // cycle (compile/hot-swap thread) before publishing the new bytecode,
    // so the chunked BufferPool grows to fit the program. Zero on
    // unsuccessful compiles.
    std::uint32_t required_buffers = 0;

    // Per-bus scratch buffer index map (prd-bus-routing). Empty for programs
    // with no audio sink. Lets a host read an individual bus's output buffer
    // by index after process_block. See CodeGenerator::emit_bus_epilogue.
    std::vector<BusBufferMapping> bus_buffers;
};

/// Per-call resource requests the host must satisfy before resuming the
/// audio thread (samples, soundfonts, MIDI sources, wavetables, URIs).
struct RuntimeRequests {
    std::vector<std::string> required_samples;  // Sample names used (for runtime loading) - legacy
    std::vector<RequiredSample> required_samples_extended;  // Sample refs with bank/variant info
    std::vector<ScalarSampleMapping> scalar_sample_mappings;  // Direct sample("name") calls needing runtime ID patching
    std::vector<RequiredSoundFont> required_soundfonts;  // SoundFont files needed at runtime
    // Per-call midi() source configs (PRD prd-midi-input §4.7). Each entry tells
    // the host to call vm.init_midi_queue_state(state_id, kind, name_or_path,
    // channel_filter, loop, tempo_mode) before resuming the audio thread, plus
    // (for File kind) load the .mid bytes via cedar_load_midi_file / UriResolver.
    std::vector<RequiredMidiSource> required_midi_sources;
#ifdef CEDAR_HOST_EXTENSIONS
    // Per-call-site hosted-node records (prd-host-extension-api §6.3). Each entry
    // tells the host to bind an instance for `state_id` — off the audio thread,
    // before resuming it. An unbound state_id makes the node pass through.
    std::vector<RequiredHostNode> required_host_nodes;
#endif
    // Per-call midi_cc() routes (PRD prd-midi-input §4.8). Compile-time directives
    // that the host MIDI callback evaluates to call vm.set_param(name, value, slew).
    std::vector<RequiredMidiCcRoute> required_midi_cc_routes;
    // Source strings collected from in('...') calls in compile order (one entry per call,
    // empty string if the call had no argument). Hosts use this to switch input source.
    std::vector<std::string> required_input_sources;
    // Wavetable banks declared via wt_load(). v1 keeps the *last* loaded bank
    // active; multi-bank routing is a v2 follow-up.
    std::vector<RequiredWavetable> required_wavetables;
    // URIs declared via top-level directives like samples("..."). Hosts iterate
    // these in source order, dispatch each by `kind` to the appropriate
    // registry, and block bytecode swap until every URI resolves.
    std::vector<UriRequest> required_uris;
};

/// UI-bearing declarations and post-compile tooling outputs.
struct CompileArtifacts {
    std::vector<ParamDecl> param_decls;  // Declared parameters for UI generation
    std::vector<VisualizationDecl> viz_decls;  // Declared visualizations for UI generation
    std::vector<BuiltinVarOverride> builtin_var_overrides;  // Builtin variable overrides (bpm, sr)

    // Phase 2 records-system-unification: analyzer outputs retained for
    // downstream tooling (e.g. shape index serialization). Populated by
    // compile() whenever the corresponding pass runs, including on early-
    // return paths so partial state can still be inspected. Not consumed by
    // the audio runtime; safe to ignore for normal compile-and-play.
    std::optional<SymbolTable> symbols;
    std::shared_ptr<Ast>       ast;

    // Hardening Phase 8 (PRD-2): inputs for serialize_shape_index().
    // `parsed_ast` is the pre-analysis parse tree of the combined
    // stdlib+modules+user source — method-call chains are still intact
    // here (the analyzer's transformed AST above desugars them to Calls).
    // `user_source_offset` is the byte offset where the user-source
    // region begins in the combined source (the user region is always
    // the final segment); `user_source_hash` is the FNV-1a hash of the
    // raw caller-provided source, matching the editor's JS-side cache
    // key. `interner` points into the compile's context — valid as long
    // as `owned_ctx` (or the caller's ctx) lives, same rule as `symbols`.
    std::shared_ptr<Ast>  parsed_ast;
    std::uint32_t         user_source_offset = 0;
    std::uint32_t         user_source_hash = 0;
    const StringInterner* interner = nullptr;

    // PRD prd-parser-codegen-correctness.md Phase 5 (F12): the
    // `symbols` SymbolTable above holds a non-owning pointer to a
    // StringInterner inside this context. When `compile()` constructs
    // a default context internally, it stashes it here so the
    // interner outlives the function return — otherwise
    // `r.artifacts.symbols->lookup(name)` would dereference a destroyed
    // interner. Callers that pass their own `CompileContext*` to
    // `compile()` leave this empty; their context owns the lifetime.
    std::shared_ptr<CompileContext> owned_ctx;
};

/// Compilation result (prd-parser-codegen-hardening Phase 2 grouped shape).
struct CompileResult {
    bool success = false;
    CompiledProgram program;
    RuntimeRequests requests;
    CompileArtifacts artifacts;
    std::vector<Diagnostic> diagnostics;
};

/// All optional inputs to a compile, grouped into one struct
/// (prd-parser-codegen-hardening Phase 2). Use designated initializers:
/// `compile(src, {.lint_strict = true})`.
struct CompileOptions {
    std::string_view filename = "<input>";
    /// Optional sample registry for resolving sample names to IDs.
    SampleRegistry* sample_registry = nullptr;
    /// Optional file resolver for import statements.
    const FileResolver* resolver = nullptr;
    /// Enable opt-in lint warnings (e.g. W201 dotted hole-field).
    bool lint_strict = false;
    /// Test-only: suppress the bus-routing master bus so out()/bus() write
    /// raw to the device (no soft-clip / safety stage). Not reachable from
    /// user source — the safety guarantee still holds.
    bool bypass_master = false;
    /// Emit per-pattern debug JSON (StateInitData::ast_json for the web
    /// pattern-debug panel). Default off: headless hosts (CLI render) pay
    /// no serialization cost. The web/wasm host sets this to true.
    bool emit_debug_json = false;
    /// Optional per-compile context (owns VoicingRegistry + StringInterner).
    /// When null, a fresh heap-owned context is constructed and stashed in
    /// `result.artifacts.owned_ctx`. Pass the same ctx across multiple
    /// compile() calls to share user-defined voicings (e.g. for live-coding
    /// hosts that want cross-edit persistence). See PRD
    /// prd-parser-codegen-correctness.md Phase 4.
    CompileContext* ctx = nullptr;
};

/// Compile Akkado source code to Cedar bytecode
CompileResult compile(std::string_view source, const CompileOptions& opts = {});

/// Compile from file. If `opts.resolver` is null, a FilesystemResolver for
/// the file's directory is created. `opts.filename` is ignored (the path is
/// used).
CompileResult compile_file(const std::string& path, const CompileOptions& opts = {});

} // namespace akkado
