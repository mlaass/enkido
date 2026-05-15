#pragma once

#include "ast.hpp"
#include "builtins.hpp"
#include "diagnostics.hpp"
#include "mini_token.hpp"
#include "required_sample.hpp"
#include "symbol_table.hpp"
#include "sample_registry.hpp"
#include "typed_value.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/opcodes/sequence.hpp>
#include <cedar/opcodes/dsp_state.hpp>
#include <cedar/opcodes/midi.hpp>
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace akkado {

// ============================================================================
// Compiler Options (set by directives like $polyphony)
// ============================================================================

/// Compiler options that can be configured via directives
struct CompilerOptions {
    std::uint8_t default_polyphony = 4;  // Default voice count for polyphonic patterns
};

// ============================================================================
// Parameter Exposure System
// ============================================================================

/// Type of exposed parameter for UI generation
enum class ParamType : std::uint8_t {
    Continuous = 0,  // Float value in range [min, max] - rendered as slider
    Button = 1,      // Momentary: 1 while pressed, 0 otherwise
    Toggle = 2,      // Boolean: 0 or 1, click to flip
    Select = 3       // Discrete: integer index into options array
};

/// Declaration of an exposed parameter extracted at compile time
/// Used for auto-generating UI controls and external binding (Godot, MIDI, etc.)
struct ParamDecl {
    std::string name;              // Display name and EnvMap key
    std::uint32_t name_hash = 0;   // FNV-1a hash for ENV_GET lookup
    ParamType type = ParamType::Continuous;
    float default_value = 0.0f;    // Initial value
    float min_value = 0.0f;        // Minimum (Continuous only)
    float max_value = 1.0f;        // Maximum (Continuous only)
    std::vector<std::string> options;  // Option names (Select only)

    // Source location for UI linking (click-to-source)
    std::uint32_t source_offset = 0;   // Byte offset in source
    std::uint32_t source_length = 0;   // Length in source
};

// ============================================================================
// Builtin Variable Overrides
// ============================================================================

/// Override for a builtin variable extracted at compile time (e.g., bpm = 120)
struct BuiltinVarOverride {
    std::string name;        // "bpm"
    float value;             // 120.0f
    SourceLocation location;
};

// ============================================================================
// Visualization Exposure System
// ============================================================================

/// Type of visualization widget for UI generation
enum class VisualizationType : std::uint8_t {
    PianoRoll = 0,   // Musical pattern display with notes/events
    Oscilloscope = 1, // Time-domain waveform (short window, real-time)
    Waveform = 2,     // Time-domain waveform (longer window)
    Spectrum = 3,     // Frequency-domain FFT display
    Waterfall = 4     // Scrolling spectrogram
};

/// Declaration of a visualization widget extracted at compile time
/// Used for generating UI visualization blocks below source lines
struct VisualizationDecl {
    std::string name;              // Display name
    VisualizationType type = VisualizationType::PianoRoll;
    std::uint32_t state_id = 0;    // For probe-based visualizations (oscilloscope, etc.)
    std::string options_json;      // Optional JSON configuration string

    // Source location for placing widget below the correct line
    std::uint32_t source_offset = 0;
    std::uint32_t source_length = 0;

    // For PianoRoll: link to the pattern's state_init index
    // (allows widget to access pattern events without duplication)
    std::int32_t pattern_state_init_index = -1;
};

// ============================================================================
// State Initialization Data
// ============================================================================

/// Sample name mapping for SequenceProgram deferred resolution
/// Tracks which events in which sequences need sample ID resolution
struct SequenceSampleMapping {
    std::uint16_t seq_idx;    // Index into sequences vector
    std::uint16_t event_idx;  // Index into sequence's events array
    std::uint8_t value_slot = 0;  // Which values[] slot to write the resolved ID into
                                  // (for merged sample polyrhythms: [bd, hh] writes
                                  // bd_id to values[0] and hh_id to values[1])
    std::string sample_name;  // Sample name to resolve
    std::string bank;         // Bank name (empty = default)
    std::uint8_t variant = 0; // Variant index (0 = first variant)
};

/// State initialization data for TIMELINE / SEQPAT_QUERY / poly / extended-
/// parameter opcodes. Emitted by codegen, consumed by program_loader and
/// the WASM `cedar_apply_state_inits` shim after the program is loaded.
struct StateInitData {
    std::uint32_t state_id;  // Must match Instruction::state_id (32-bit FNV-1a hash)
    enum class Type : std::uint8_t {
        Timeline = 1,     // Initialize TimelineState with breakpoints
        SequenceProgram,  // Initialize SequenceState with compiled sequences
        PolyAlloc,        // Initialize PolyAllocState for poly/mono/legato
        ExtendedParams,   // Initialize ExtendedParams<N> with constants/buffer refs
    } type;

    // Cycle length in beats (used by SequenceProgram)
    float cycle_length = 4.0f;

    // For Timeline: [time, value, curve, ...] triplets (existing usage)

    // For SequenceProgram: compiled sequences for lazy query
    // Note: The Sequence structs contain pointers to event vectors stored here
    std::vector<cedar::Sequence> sequences;  // Compiled sequence data (shallow copy)
    std::vector<std::vector<cedar::Event>> sequence_events;  // Actual event storage per sequence
    bool is_sample_pattern = false;  // Sample pattern vs pitch pattern

    // Size hints for arena allocation
    std::uint32_t total_events = 0;  // Total event count across all sequences

    // For SequenceProgram: sample name mappings for deferred resolution
    std::vector<SequenceSampleMapping> sequence_sample_mappings;

    // Pattern string location in document (for UI highlighting)
    SourceLocation pattern_location;  // Document offset of pattern string

    // AST JSON (serialized during compilation for debug UI)
    std::string ast_json;

    // For Timeline: breakpoint data
    std::vector<cedar::TimelineState::Breakpoint> timeline_breakpoints;
    bool timeline_loop = false;
    float timeline_loop_length = 0.0f;

    // For PolyAlloc: configuration for poly/mono/legato voice allocation
    std::uint32_t poly_seq_state_id = 0;   // Linked SequenceState for events
    std::uint8_t poly_max_voices = 8;
    std::uint8_t poly_mode = 0;            // 0=poly, 1=mono, 2=legato
    std::uint8_t poly_steal_strategy = 0;  // 0=oldest

    // For SequenceProgram: iter()/iterBack() rotation configuration.
    // iter_n=0 disables rotation. iter_dir is +1 for iter, -1 for iterBack.
    // Hosts should call vm.init_sequence_iter_state() after
    // init_sequence_program_state() when iter_n > 0.
    std::uint8_t iter_n = 0;
    std::int8_t iter_dir = 0;

    // For ExtendedParams: per-slot constant value + buffer index. A slot
    // with buffer_idx == 0xFFFF reads the constant; otherwise the runtime
    // reads from the buffer at each sample. Mirrors how inputs[] work.
    // See cedar/include/cedar/opcodes/dsp_state.hpp ExtendedParams<N>.
    std::array<float, MAX_EXTENDED_PARAMS> ext_constants = {};
    std::array<std::uint16_t, MAX_EXTENDED_PARAMS> ext_buffer_indices = {};
    std::uint8_t ext_count = 0;
};

/// Required SoundFont from compile-time soundfont() calls
struct RequiredSoundFont {
    std::string filename;     // SF2 filename (from string literal)
    int preset_index = 0;    // Preset index within the SF2
};

/// Required MIDI source from compile-time midi() calls
/// (PRD prd-midi-input §4.7). One entry per midi() call site — duplicates by
/// `device:` or `file:` are NOT collapsed, because per-call channel filters
/// and option overrides may differ. The host iterates this list after
/// compilation and calls `vm.init_midi_queue_state(...)` with every field.
struct RequiredMidiSource {
    std::uint32_t              state_id = 0;
    cedar::MidiSourceKind      kind = cedar::MidiSourceKind::DefaultDevice;
    std::string                name_or_path;   // empty for DefaultDevice
    std::uint8_t               channel_filter = 0;  // 0 = any, 1..16
    bool                       loop = false;
    cedar::MidiQueueState::TempoMode tempo_mode =
        cedar::MidiQueueState::TempoMode::Follow;
};

/// Required MIDI CC / pitch-bend / channel-aftertouch → param() route from
/// a compile-time `midi_cc(...)` directive (PRD prd-midi-input §4.8).
/// Emits no bytecode; the host MIDI callback walks this table and calls
/// `vm.set_param(param_name, value, slew_ms)` for matching events. The
/// audio thread sees the result through the existing EnvMap interpolated
/// path. cc_num sentinels: 0..127 = MIDI CC number; -1 = pitch-bend
/// (14-bit, status 0xEn); -2 = channel aftertouch (status 0xDn).
struct RequiredMidiCcRoute {
    std::string   param_name;
    std::int16_t  cc_num         = 0;     // 0..127 = CC#, -1 = PB, -2 = AT
    std::uint8_t  channel_filter = 0;     // 0 = any, 1..16
    float         scale          = 1.0f;  // max - min
    float         bias           = 0.0f;  // min
    float         slew_ms        = 5.0f;  // EnvMap default
};

/// Required wavetable bank from a compile-time wt_load(...) directive.
/// The host iterates these after compilation in order: native loads each
/// via `VM::wavetable_registry().load_from_file(name, path)`; WASM fetches
/// each WAV and feeds it through cedar_load_wavetable_wav(name, bytes).
///
/// The compiler assigns slot IDs by source order (first wt_load = 0,
/// second = 1, …). The runtime registry MUST be cleared before loading
/// these in order so its sequential IDs match. Each `smooch("name", ...)`
/// call resolves its bank ID at compile time via name lookup against this
/// list, and the assigned ID is encoded into the OSC_WAVETABLE
/// instruction's rate field.
struct RequiredWavetable {
    std::string name;     // Bank identifier referenced by smooch("...")
    std::string path;     // Filesystem path (ignored on WASM)
    int         id = 0;   // Slot ID assigned by the compiler (== index in this vector)
};

/// Asset category for a UriRequest. The host dispatches on this to decide
/// which registry receives the resolved bytes.
enum class UriKind {
    SampleBank,   // strudel.json manifest → BankRegistry.loadBank
    SoundFont,    // .sf2/.sf3 → SoundFontRegistry
    Wavetable,    // .wav → WavetableBankRegistry
    Sample        // single .wav/.ogg/.flac → SampleBank
};

/// Required asset declared via a compile-time URI directive (e.g.
/// `samples("github:...")`). The host iterates these in source order after
/// compilation, fetches each via `cedar::UriResolver` / `uriResolver`, and
/// feeds the bytes into the registry indicated by `kind`. Bytecode swap
/// blocks until every URI resolves (success or terminal failure).
struct UriRequest {
    std::string uri;
    UriKind     kind = UriKind::SampleBank;
};

/// Result of code generation
struct CodeGenResult {
    std::vector<cedar::Instruction> instructions;
    std::vector<SourceLocation> source_locations;  // Parallel to instructions, tracks origin
    std::vector<Diagnostic> diagnostics;
    std::vector<StateInitData> state_inits;  // State initialization data
    std::vector<std::string> required_samples;  // Unique sample names used - legacy
    std::vector<RequiredSample> required_samples_extended;  // Sample refs with bank/variant info
    std::vector<ScalarSampleMapping> scalar_sample_mappings;  // Direct sample("name") references requiring runtime ID patching
    std::vector<RequiredSoundFont> required_soundfonts;  // SoundFont files needed at runtime
    std::vector<RequiredMidiSource> required_midi_sources;  // Per-call MIDI source configs (PRD prd-midi-input)
    std::vector<RequiredMidiCcRoute> required_midi_cc_routes;  // midi_cc() routes (PRD prd-midi-input §4.8)
    // Input source strings collected from in('...') calls (per-call, not deduplicated).
    // Empty means in() was called with no argument (host uses UI default).
    // Hosts use this metadata to switch input source on compile.
    std::vector<std::string> required_input_sources;
    std::vector<ParamDecl> param_decls;  // Declared parameters for UI generation
    std::vector<VisualizationDecl> viz_decls;  // Declared visualizations for UI generation
    std::vector<BuiltinVarOverride> builtin_var_overrides;  // Builtin variable overrides (bpm, sr)
    std::vector<RequiredWavetable> required_wavetables;  // Wavetable banks from wt_load()
    std::vector<UriRequest> required_uris;  // URI declarations from samples() etc.
    bool success = false;
};

/// Buffer allocator for code generation
/// Simple linear allocation with no reuse (MVP)
class BufferAllocator {
public:
    static constexpr std::uint16_t MAX_BUFFERS = 256;
    static constexpr std::uint16_t BUFFER_UNUSED = 0xFFFF;
    // Buffer 255 is reserved for BUFFER_ZERO (always contains 0.0)
    static constexpr std::uint16_t MAX_ALLOCATABLE = 255;

    BufferAllocator() = default;

    /// Allocate a new buffer
    /// Returns BUFFER_UNUSED if pool exhausted
    [[nodiscard]] std::uint16_t allocate();

    /// Get current allocation count
    [[nodiscard]] std::uint16_t count() const { return next_; }

    /// Check if any buffers available
    [[nodiscard]] bool has_available() const { return next_ < MAX_ALLOCATABLE; }

private:
    std::uint16_t next_ = 0;
};

// Forward declaration: SequenceCompiler is defined in codegen_patterns.cpp.
// Used by emit_custom_property_buffers() to read the compile-time slot map
// without exposing the full SequenceCompiler implementation in this header.
class SequenceCompiler;

/// Code generator: converts analyzed AST to Cedar bytecode
class CodeGenerator {
public:
    /// Generate bytecode from analyzed AST
    /// @param ast The transformed AST (after pipe rewriting)
    /// @param symbols Symbol table from semantic analysis
    /// @param filename Filename for error reporting
    /// @param sample_registry Optional sample registry for resolving sample names to IDs
    /// @param source_map Optional source map for module-aware semantic ID paths
    CodeGenResult generate(const Ast& ast, SymbolTable& symbols,
                          std::string_view filename = "<input>",
                          SampleRegistry* sample_registry = nullptr,
                          const class SourceMap* source_map = nullptr);

private:
    /// Visit AST node and emit instructions
    /// Returns a TypedValue with buffer index and type metadata
    TypedValue visit(NodeIndex node);

    /// Cache a TypedValue for a node and return it
    TypedValue cache_and_return(NodeIndex node, TypedValue tv) {
        node_types_[node] = tv;
        return tv;
    }

    /// Emit a single instruction
    void emit(const cedar::Instruction& inst);

    /// Generate semantic ID from path
    [[nodiscard]] std::uint32_t compute_state_id() const;

    /// Push/pop semantic path for nested structures
    void push_path(std::string_view segment);
    void pop_path();

    /// Error helpers
    void error(const std::string& code, const std::string& message, SourceLocation loc);

    /// Warning helper - emits a warning diagnostic but does not stop compilation
    void warn(const std::string& code, const std::string& message, SourceLocation loc);

    /// Check if a match expression can be resolved at compile time
    /// Returns true if scrutinee (if present) is const and all patterns/guards are const
    [[nodiscard]] bool is_compile_time_match(NodeIndex node, const Node& n) const;

    /// Handle compile-time match - evaluate patterns and guards, emit only winning branch
    TypedValue handle_compile_time_match(NodeIndex node, const Node& n);

    /// Handle runtime match - emit all branches and build nested select chain
    TypedValue handle_runtime_match(NodeIndex node, const Node& n);

    /// Get the TypedValue for a node (checks node_types_ cache, then follows symbol table)
    /// Returns nullptr if node has no typed value
    const TypedValue* get_node_type(NodeIndex node) const;

    /// Bind destructured fields from a record/pattern TypedValue into the symbol table.
    /// Returns true if all fields were bound, false on error.
    ///
    /// Each `DestructureField` carries a name and an optional `default_node`
    /// (the AST index of a default expression). When the source is missing a
    /// field AND the field has a `default_node`, codegen visits the default
    /// expression and binds the resulting buffer to the field name; when the
    /// field has no default, `missing_field_code` fires.
    ///
    /// `missing_field_code` defaults to `"E141"` for the existing
    /// pipe-binding (`as {x, y}`) and match-arm paths; the new statement-level
    /// (`{x, y} = …`) and fn-param destructure paths pass `"E187"` per
    /// `prd-records-system-unification.md` §10.0.
    bool bind_destructure_fields(const TypedValue& source_tv,
                                 const std::vector<DestructureField>& fields,
                                 SourceLocation loc,
                                 const char* missing_field_code = "E141");

    /// FM Detection: Automatically upgrade oscillators to 4x when FM is detected
    /// @param freq_buffer The buffer index containing the frequency input
    /// @return true if the frequency input traces back to an audio-rate source
    /// Note: is_audio_rate_producer, is_upgradeable_oscillator, upgrade_for_fm
    /// are inline helpers in akkado/codegen/fm_detection.hpp
    [[nodiscard]] bool is_fm_modulated(std::uint16_t freq_buffer) const;

    /// Handle len() function calls - compile-time array length
    TypedValue handle_len_call(NodeIndex node, const Node& n);

    /// Handle state(init) calls - allocate a CellState slot, return a StateCell
    /// TypedValue carrying the slot's state_id (per-call-site path-hash).
    TypedValue handle_state_call(NodeIndex node, const Node& n);

    /// Handle get(s) - emit STATE_OP rate=1 broadcasting cell value to a buffer.
    /// First arg must be a StateCell.
    TypedValue handle_get_call(NodeIndex node, const Node& n);

    /// Handle set(s, v) - emit STATE_OP rate=2 storing the last sample of v
    /// into the cell. First arg must be a StateCell. Returns the new value
    /// broadcast as a Signal so set() can be used in expression position.
    TypedValue handle_set_call(NodeIndex node, const Node& n);

    /// Phase 4a (records-system-unification): record-valued state cells.
    ///
    /// A `state({x: 1, y: 2})` cell is fanned out into N scalar STATE_OP slots,
    /// one per record field, keyed by per-field path hashes. The returned
    /// StateCell TypedValue carries a RecordPayload whose fields map each
    /// field name to a sub-StateCell carrying that field's state_id.
    ///
    /// Field names are sorted alphabetically before fan-out so that codegen
    /// is deterministic across compiles (unordered_map iteration is not),
    /// which keeps hot-swap diffs stable.
    ///
    /// Caller of handle_state_record_init has already pushed the parent
    /// `state#N` path; the helper pushes/pops each field name in turn.
    TypedValue handle_state_record_init(NodeIndex node, const Node& n,
                                         const TypedValue& init_tv);

    /// Fan out a record-valued cell into N STATE_OP rate=1 loads; assemble
    /// a Record TypedValue (one Signal per field). Existing field-access
    /// codegen handles `get(cell).x` through the returned Record.
    TypedValue handle_get_record_cell(NodeIndex node, const Node& n,
                                        const TypedValue& cell_tv);

    /// Fan out a record-valued cell `set` into N STATE_OP rate=2 stores.
    /// Caller has already shape-validated `value_tv` against `cell_tv`.
    /// Returns a Record TypedValue (one Signal per field) so callers
    /// chaining on `set()` see the new value's shape.
    TypedValue handle_set_record_cell(NodeIndex node, const Node& n,
                                        const TypedValue& cell_tv,
                                        const TypedValue& value_tv);

    /// Argument-spread expansion (record/array spread in function calls).
    /// One entry per logical argument the call site provides. For concrete
    /// args, source_node points at the value AST and resolved is empty. For
    /// args produced by spreading a record/array, resolved holds the
    /// pre-evaluated TypedValue and source_node is NULL_NODE.
    struct ExpandedArg {
        std::optional<std::string> name;     // None → positional
        NodeIndex source_node = NULL_NODE;   // Concrete value AST, or NULL_NODE if spread-resolved
        std::optional<TypedValue> resolved;  // Pre-evaluated TypedValue from spread expansion
        SourceLocation loc;
    };

    /// Walks the call's children, expanding any ..record / ..array spread
    /// argument by visiting its source once and emitting one ExpandedArg per
    /// field/element. Concrete (named or positional) args pass through as-is.
    /// Emits E140 (non-record/non-array source) and E180 (record + array
    /// spread mixed in one call) on failure.
    /// Returns nullopt on hard error.
    std::optional<std::vector<ExpandedArg>> expand_call_arguments(NodeIndex call_node);

    /// Handle user-defined function calls - inline expansion
    TypedValue handle_user_function_call(NodeIndex node, const Node& n,
                                         const UserFunctionInfo& func);

    /// Handle FunctionValue calls - inline expansion of lambda assigned to variable
    TypedValue handle_function_value_call(NodeIndex node, const Node& n,
                                           const FunctionRef& func);

    /// Handle Closure nodes - allocate buffers for parameters and generate body
    TypedValue handle_closure(NodeIndex node, const Node& n);

    /// Handle MatchExpr nodes - compile-time match resolution
    TypedValue handle_match_expr(NodeIndex node, const Node& n);

    /// Handle pattern variable reference
    /// Emits pattern code for the stored pattern.
    TypedValue handle_pattern_reference(const std::string& name, NodeIndex pattern_node,
                                        SourceLocation loc);

    /// Handle chord() function calls - Strudel-compatible chord expansion
    TypedValue handle_chord_call(NodeIndex node, const Node& n);

    /// PRD prd-patterns-as-scalar-values: explicit call-form for the typed prefixes.
    /// value("…") parses with MiniParseMode::Value (numeric atoms, no mtof).
    /// note("…")  parses with MiniParseMode::Note  (note names + bare MIDI ints).
    /// scalar(p)  is the explicit Pattern→Signal cast (sugar for p.freq on
    /// monophonic non-sample patterns; idempotent on Signal arg).
    TypedValue handle_value_call(NodeIndex node, const Node& n);
    TypedValue handle_note_call(NodeIndex node, const Node& n);
    TypedValue handle_scalar_call(NodeIndex node, const Node& n);

    /// Helper: shared body for chord/value/note pattern calls — parses the
    /// string in the requested mode and emits SEQPAT machinery. The mode
    /// determines per-atom semantics; payload flags reflect the result.
    TypedValue compile_typed_pattern_call(NodeIndex node, const Node& n,
                                          MiniParseMode mode,
                                          std::string_view fn_name);

    /// Handle MiniLiteral (pattern) nodes - pat("c4 e4 g4"), etc.
    TypedValue handle_mini_literal(NodeIndex node, const Node& n);

    /// Handle timeline curve literal nodes - t"__/'", etc.
    TypedValue handle_timeline_literal(NodeIndex node, const Node& n);

    /// Handle timeline("...") function call form
    TypedValue handle_timeline_call(NodeIndex node, const Node& n);

    /// Emit per-voice SEQPAT_STEP/GATE/TYPE instructions and register polyphonic fields
    /// Shared helper used by handle_mini_literal and handle_pattern_reference
    /// @param node The node to register polyphonic fields / multi-buffers on
    /// @param state_id The state_id for the SEQPAT instructions
    /// @param max_voices Number of voices to emit
    /// @param value_buf Pre-allocated buffer for voice 0 value
    /// @param velocity_buf Pre-allocated buffer for voice 0 velocity
    /// @param trigger_buf Pre-allocated buffer for voice 0 trigger
    /// @param is_sample_pattern Whether this is a sample pattern (skips polyphonic registration)
    /// @param loc Source location for error reporting
    /// @return PatternPayload on success, nullptr on buffer exhaustion
    std::shared_ptr<PatternPayload> emit_per_voice_seqpat(
        NodeIndex node, std::uint32_t state_id,
        std::uint8_t max_voices,
        std::uint16_t value_buf, std::uint16_t velocity_buf,
        std::uint16_t trigger_buf,
        bool is_sample_pattern, SourceLocation loc,
        std::uint16_t clock_override = 0xFFFF);

public:
    /// Phase 2.1 PRD §11: emit one SEQPAT_PROP instruction per registered
    /// custom property slot in `compiler`, allocate a buffer per slot, and
    /// populate `payload->custom_fields` so `e.<key>` resolves to the buffer.
    /// Returns false on buffer exhaustion (caller should emit E101).
    /// Public so that the free static `emit_pattern_with_state()` helper in
    /// codegen_patterns.cpp can call it.
    bool emit_custom_property_buffers(
        const SequenceCompiler& compiler,
        PatternPayload& payload,
        std::uint32_t state_id,
        std::uint16_t clock_override = 0xFFFF);

    /// Allocate the 8 extended pattern-field buffers (gate, type, note, dur,
    /// chance, time, phase, sample_id), emit SEQPAT_GATE/TYPE/FIELD/PHASE for
    /// voice 0, and assign them into payload->fields[]. Closes records-and-
    /// field-access PRD §3 by making the extended fields available on every
    /// pattern producer, not just mini-notation literals.
    /// Returns false on buffer exhaustion (caller should emit E101).
    bool emit_extended_field_buffers(
        PatternPayload& payload,
        std::uint32_t state_id,
        SourceLocation loc,
        std::uint16_t clock_override = 0xFFFF);

    /// Read-only access to the symbol table for free helpers in
    /// codegen_patterns.cpp (e.g. compile_pattern_for_transform resolving
    /// identifier-bound patterns through PatternInfo.pattern_node).
    const SymbolTable& symbols() const { return *symbols_; }

    /// Publish a Pattern's `sample_refs` to the global
    /// `required_samples_extended_` ledger, deduplicated by `RequiredSample::key()`.
    /// Single source of registration: every Pattern producer calls this after
    /// building its `PatternPayload`. Public so the free static helper
    /// `emit_pattern_with_state()` in codegen_patterns.cpp can invoke it
    /// through its `gen` parameter.
    void publish_sample_refs(const std::vector<RequiredSample>& refs);

private:

    // ============================================================================
    // Pattern transformation handlers
    // ============================================================================

    /// Handle slow(pattern, factor) - stretch pattern by factor
    TypedValue handle_slow_call(NodeIndex node, const Node& n);

    /// Handle fast(pattern, factor) - compress pattern by factor
    TypedValue handle_fast_call(NodeIndex node, const Node& n);

    /// Handle rev(pattern) - reverse event order
    TypedValue handle_rev_call(NodeIndex node, const Node& n);

    /// Handle transpose(pattern, semitones) - shift pitches by semitones
    TypedValue handle_transpose_call(NodeIndex node, const Node& n);

    /// Handle velocity(pattern, vel) - set velocity on all events
    TypedValue handle_velocity_call(NodeIndex node, const Node& n);

    /// Phase 2.1 PRD §11.2: Handle bend(pattern, value) — set bend property
    /// on all events. Surfaced as `e.bend` after pipe-binding.
    TypedValue handle_bend_call(NodeIndex node, const Node& n);

    /// Phase 2.1 PRD §11.2: Handle aftertouch(pattern, value) — set aftertouch
    /// property on all events. Surfaced as `e.aftertouch` after pipe-binding.
    TypedValue handle_aftertouch_call(NodeIndex node, const Node& n);

    /// Phase 2.1 PRD §11.2: Handle dur(pattern, factor) — multiply event
    /// durations by factor. Pure compile-time mutation of cedar::Event.duration.
    TypedValue handle_dur_call(NodeIndex node, const Node& n);

    /// Phase 2.1 PRD §11.2: shared backbone for bend/aftertouch handlers.
    /// `key` is the slot name (e.g. "bend", "aftertouch") used both for
    /// SequenceCompiler slot allocation and `e.<key>` accessor resolution.
    TypedValue handle_property_transform_call(
        NodeIndex node, const Node& n, const std::string& key);

    /// Handle bank(pattern, bank_name) - set sample bank for all events
    TypedValue handle_bank_call(NodeIndex node, const Node& n);

    /// Handle variant(pattern, index) - set sample variant for all events
    TypedValue handle_variant_call(NodeIndex node, const Node& n);

    /// Handle transport(pattern, trig, step?, reset?) - trigger-driven pattern clock
    TypedValue handle_transport_call(NodeIndex node, const Node& n);

    /// Handle tune(tuning_name, pattern) - apply microtonal tuning to pattern
    TypedValue handle_tune_call(NodeIndex node, const Node& n);

    /// Handle early(pattern, amount) - shift events earlier (wraps within cycle)
    TypedValue handle_early_call(NodeIndex node, const Node& n);

    /// Handle late(pattern, amount) - shift events later (wraps within cycle)
    TypedValue handle_late_call(NodeIndex node, const Node& n);

    /// Handle palindrome(pattern) - forward then reversed (doubles cycle length)
    TypedValue handle_palindrome_call(NodeIndex node, const Node& n);

    /// Handle compress(pattern, start, end) - squash events into [start, end)
    TypedValue handle_compress_call(NodeIndex node, const Node& n);

    /// Handle ply(pattern, n) - replace each event with n equal-duration copies
    TypedValue handle_ply_call(NodeIndex node, const Node& n);

    /// Handle linger(pattern, frac) - keep first frac of pattern, loop to fill cycle
    TypedValue handle_linger_call(NodeIndex node, const Node& n);

    /// Handle zoom(pattern, start, end) - keep events in [start, end), stretch to fill
    TypedValue handle_zoom_call(NodeIndex node, const Node& n);

    /// Handle segment(pattern, n) - sample at n evenly-spaced points
    TypedValue handle_segment_call(NodeIndex node, const Node& n);

    /// Handle swing(pattern, n=4) - 1/3 swing on n-slice grid
    TypedValue handle_swing_call(NodeIndex node, const Node& n);

    /// Handle swingBy(pattern, amount, n=4) - custom-amount swing on n-slice grid
    TypedValue handle_swing_by_call(NodeIndex node, const Node& n);

    /// Handle iter(pattern, n) - rotate pattern start by 1/n per cycle (forward)
    TypedValue handle_iter_call(NodeIndex node, const Node& n);

    /// Handle iterBack(pattern, n) - rotate pattern start by 1/n per cycle (backward)
    TypedValue handle_iter_back_call(NodeIndex node, const Node& n);

    /// Handle run(n) - integer pattern 0..n-1 evenly spaced
    TypedValue handle_run_call(NodeIndex node, const Node& n);

    /// Handle binary(n) - trigger pattern from binary representation of n
    TypedValue handle_binary_call(NodeIndex node, const Node& n);

    /// Handle binaryN(n, bits) - trigger pattern from low `bits` bits of n
    TypedValue handle_binary_n_call(NodeIndex node, const Node& n);

    /// Handle anchor(pattern, "c4") - set voicing anchor MIDI note
    TypedValue handle_anchor_call(NodeIndex node, const Node& n);

    /// Handle mode(pattern, "below") - set chord voicing mode
    TypedValue handle_mode_call(NodeIndex node, const Node& n);

    /// Handle voicing(pattern, "drop2") - apply named voicing dictionary
    TypedValue handle_voicing_call(NodeIndex node, const Node& n);

    /// Handle addVoicings(name, {...}) - register a custom voicing dictionary
    TypedValue handle_add_voicings_call(NodeIndex node, const Node& n);

    /// Handle tap_delay(in, time, fb, processor) - tap delay with inline feedback chain
    /// Emits DELAY_TAP, compiles processor closure inline, then emits DELAY_WRITE
    TypedValue handle_tap_delay_call(NodeIndex node, const Node& n);

    /// Handle soundfont(file, preset) - SoundFont playback
    /// Special-cased: extracts filename/preset at compile time, emits SOUNDFONT_VOICE
    TypedValue handle_soundfont_call(NodeIndex node, const Node& n);

    /// Handle midi(options?) - runtime MIDI event source (PRD prd-midi-input §4.7).
    /// Special-cased: emits one MIDI_QUERY, records a RequiredMidiSource for the
    /// host to call vm.init_midi_queue_state(state_id, ...) with after load.
    /// Returns a TypedValue::EventSource that handle_poly_call accepts as
    /// upstream alongside Pattern.
    TypedValue handle_midi_call(NodeIndex node, const Node& n);

    /// Handle midi_cc("param_name", options) — compile-time CC / pitch-bend /
    /// aftertouch route to a param() slot (PRD prd-midi-input §4.8). Emits no
    /// instruction; records a RequiredMidiCcRoute entry the host MIDI callback
    /// evaluates at runtime to call vm.set_param(name, value, slew_ms).
    TypedValue handle_midi_cc_call(NodeIndex node, const Node& n);

    /// Handle wt_load("name", "path") - register a wavetable bank.
    /// Compile-time directive only — emits no instruction. Both args must
    /// be string literals. The (name, path, id) tuple is recorded in
    /// `required_wavetables_`; the host loads it after compilation.
    TypedValue handle_wt_load_call(NodeIndex node, const Node& n);

    /// Handle samples("uri") - declare a sample-bank URI for the host to load.
    /// Compile-time directive only — emits no instruction. The argument must
    /// be a string literal (any URI scheme: file://, https://, github:, ...).
    /// Recorded in `required_uris_` with kind = SampleBank; the host fetches
    /// each entry before bytecode swap.
    TypedValue handle_samples_call(NodeIndex node, const Node& n);

    /// Handle smooch("bank", freq, phase?, tablePos?) - wavetable oscillator.
    /// First arg must be a string literal naming a previously declared
    /// wt_load bank. The bank ID is resolved at compile time and packed
    /// into inst.rate; freq/phase/tablePos lower as audio-rate signals.
    TypedValue handle_smooch_call(NodeIndex node, const Node& n);

    /// Handle in() / in(source) - live audio input
    /// Allocates an adjacent buffer pair and emits INPUT. Optional source string
    /// literal is recorded in required_input_sources_ for host metadata routing.
    TypedValue handle_input_call(NodeIndex node, const Node& n);

    /// Handle poly(voices, instrument) / mono(instrument) / legato(instrument)
    /// Emits POLY_BEGIN, inlines instrument body, emits POLY_END
    TypedValue handle_poly_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Stereo handlers
    // ============================================================================

    /// Handle stereo(mono) or stereo(left, right) - create stereo signal
    TypedValue handle_stereo_call(NodeIndex node, const Node& n);

    /// Handle mono(stereo) - downmix stereo to mono via (L+R)*0.5.
    /// Dispatches to the voice-manager form mono(instrument) when the
    /// argument resolves to a function reference instead of a signal.
    TypedValue handle_mono_call(NodeIndex node, const Node& n);

    /// Handle left(stereo) - extract left channel
    TypedValue handle_left_call(NodeIndex node, const Node& n);

    /// Handle right(stereo) - extract right channel
    TypedValue handle_right_call(NodeIndex node, const Node& n);

    /// Handle pan(mono, pos) - pan mono to stereo
    TypedValue handle_pan_call(NodeIndex node, const Node& n);

    /// Handle width(stereo, amount) - adjust stereo width (convenience wrapper)
    TypedValue handle_width_call(NodeIndex node, const Node& n);

    /// Handle ms_encode(stereo) - convert to mid/side (convenience wrapper)
    TypedValue handle_ms_encode_call(NodeIndex node, const Node& n);

    /// Handle ms_decode(ms) - convert mid/side to stereo (convenience wrapper)
    TypedValue handle_ms_decode_call(NodeIndex node, const Node& n);

    /// Handle pingpong(stereo, time, fb) - true stereo ping-pong delay
    TypedValue handle_pingpong_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Parameter exposure handlers
    // ============================================================================

    /// Handle param(name, default, min?, max?) - continuous slider parameter
    TypedValue handle_param_call(NodeIndex node, const Node& n);

    /// Handle button(name) - momentary button parameter
    TypedValue handle_button_call(NodeIndex node, const Node& n);

    /// Handle toggle(name, default?) - boolean toggle parameter
    TypedValue handle_toggle_call(NodeIndex node, const Node& n);

    /// Handle select(name, opt1, opt2, ...) - selection dropdown parameter
    TypedValue handle_select_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Visualization exposure handlers
    // ============================================================================

    /// Handle pianoroll(signal, name?) - attach piano roll visualization
    TypedValue handle_pianoroll_call(NodeIndex node, const Node& n);

    /// Handle oscilloscope(signal, name?) - attach oscilloscope visualization
    TypedValue handle_oscilloscope_call(NodeIndex node, const Node& n);

    /// Handle waveform(signal, name?) - attach waveform visualization
    TypedValue handle_waveform_call(NodeIndex node, const Node& n);

    /// Handle spectrum(signal, name?) - attach spectrum visualization
    TypedValue handle_spectrum_call(NodeIndex node, const Node& n);

    /// Handle waterfall(signal, name?, options?) - attach waterfall spectrogram visualization
    TypedValue handle_waterfall_call(NodeIndex node, const Node& n);

    /// Handle compose(f, g, ...) - function composition
    TypedValue handle_compose_call(NodeIndex node, const Node& n);

    /// Emit an ExtendedParams StateInitData entry for a builtin whose
    /// `extended_param_count > 0`. `arg_buffers` is the codegen-collected
    /// per-argument buffer list (positions 0..N-1 correspond to the
    /// builtin's parameter list, after reorder_named_arguments has run
    /// in the analyzer). Args past `total_params()` are routed into the
    /// ExtendedParams slot table; missing trailing args fall back to
    /// `extended_defaults[]` as constant slots. No-op when
    /// extended_param_count == 0.
    void emit_extended_params_init(std::uint32_t state_id,
                                   const BuiltinInfo& info,
                                   const std::vector<std::uint16_t>& arg_buffers);

    // ============================================================================
    // Directive handlers
    // ============================================================================

    /// Handle compiler directives ($polyphony, etc.)
    /// Processes the directive and updates compiler options.
    /// @return void TypedValue (directives don't produce values)
    TypedValue handle_directive(NodeIndex node, const Node& n);

    // Context
    CompilerOptions options_;  // Compiler options set by directives
    const Ast* ast_ = nullptr;
    SymbolTable* symbols_ = nullptr;
    SampleRegistry* sample_registry_ = nullptr;
    const class SourceMap* source_map_ = nullptr;
    BufferAllocator buffers_;
    std::vector<cedar::Instruction> instructions_;
    std::vector<SourceLocation> source_locations_;  // Parallel to instructions_
    std::vector<Diagnostic> diagnostics_;
    std::vector<StateInitData> state_inits_;  // State initialization data
    std::vector<ParamDecl> param_decls_;      // Declared parameters
    std::vector<VisualizationDecl> viz_decls_;  // Declared visualizations
    std::vector<BuiltinVarOverride> builtin_var_overrides_;  // Builtin var overrides
    std::string filename_;
    SourceLocation current_source_loc_;  // Current source location for emitted instructions

    // Semantic path tracking for state_id generation
    std::vector<std::string> path_stack_;
    std::uint32_t anonymous_counter_ = 0;

    // Track call counts per stateful function for unique state_ids
    std::unordered_map<std::string, std::uint32_t> call_counters_;

    // PRD prd-midi-input §10 Q4: depth counter incremented while visiting a
    // user-function / closure body. handle_midi_call rejects with E412 when
    // this is > 0 — `midi()` is top-level only in v1.
    int user_function_depth_ = 0;

    // Track unique sample names used (for runtime loading) - legacy
    std::set<std::string> required_samples_;
    // Track samples with bank/variant info for extended sample resolution
    std::set<std::string> required_samples_extended_keys_;  // For deduplication
    std::vector<RequiredSample> required_samples_extended_;
    // Direct sample("name", ...) calls record the instruction index of the
    // PUSH_CONST that holds the sample-id placeholder, plus the sample name
    // to resolve. Patched by the host after the sample bank is populated.
    std::vector<ScalarSampleMapping> scalar_sample_mappings_;
    // Track required SoundFont files
    std::vector<RequiredSoundFont> required_soundfonts_;
    // Track per-call midi() configs (PRD prd-midi-input §4.7).
    std::vector<RequiredMidiSource> required_midi_sources_;
    // Track per-call midi_cc() routes (PRD prd-midi-input §4.8).
    std::vector<RequiredMidiCcRoute> required_midi_cc_routes_;
    // Track required wavetable banks from compile-time wt_load() directives
    // (per-call order; deduplicated on (name, path) pair to avoid reloading
    // when the same bank appears in multiple modules).
    std::vector<RequiredWavetable> required_wavetables_;
    // Track URI declarations from samples()/etc. (per-call order; deduplicated
    // on uri string within each kind). Drained into CodeGenResult.required_uris.
    std::vector<UriRequest> required_uris_;
    // Track input source strings from in('...') calls (per-call order; not deduplicated)
    std::vector<std::string> required_input_sources_;

    // Map from AST node index to typed result (replaces node_buffers_, record_fields_,
    // multi_buffers_, array_lengths_, pattern_state_ids_)
    std::unordered_map<NodeIndex, TypedValue> node_types_;

    // Side-table for synthetic NodeType::PreResolved nodes inserted during
    // call-arg spread expansion (Phase 6). visit() returns this TypedValue
    // verbatim — the buffer is already allocated by the spread source's visit.
    std::unordered_map<NodeIndex, TypedValue> pre_resolved_values_;

    // Map from parameter name hash to literal AST node (for inline match resolution)
    // Only populated during user function calls when the argument is a literal
    std::unordered_map<std::uint32_t, NodeIndex> param_literals_;

    // Map from parameter name hash to string default value (for compile-time match dispatch)
    // Used when a string default parameter is not provided at call site
    std::unordered_map<std::uint32_t, std::string> param_string_defaults_;

    // Pending function ref from a closure-returning function call.
    // Set by handle_user_function_call when body is a Closure node.
    // Consumed by Assignment handler to bind as FunctionValue.
    std::optional<FunctionRef> pending_function_ref_;

    // Map from parameter name hash to FunctionRef (for closure parameters in user functions)
    // When a closure or function reference is passed as an argument to a user function,
    // this map stores the FunctionRef so it can be bound as FunctionValue in the function scope.
    std::unordered_map<std::uint32_t, FunctionRef> param_function_refs_;

    // Map from parameter name hash to argument node index (for multi-buffer propagation)
    // When a multi-buffer argument is passed to a function, this maps the parameter name
    // to the original argument node so that multi-buffer info can be looked up later.
    std::unordered_map<std::uint32_t, NodeIndex> param_multi_buffer_sources_;

    // ============================================================================
    // Stereo signal tracking
    // ============================================================================
    // Track nodes that produce stereo signals (2-buffer pairs: left, right)
    struct StereoBuffers {
        std::uint16_t left;
        std::uint16_t right;
    };
    std::unordered_map<NodeIndex, StereoBuffers> stereo_outputs_;

    // Track stereo by buffer index (for variable propagation)
    // Key is the left buffer index, value is the right buffer index
    std::unordered_map<std::uint16_t, std::uint16_t> stereo_buffer_pairs_;

    /// Register a node as producing a stereo signal (2 buffers)
    void register_stereo(NodeIndex node, std::uint16_t left, std::uint16_t right);

    /// Check if a node produces a stereo signal
    [[nodiscard]] bool is_stereo(NodeIndex node) const;

    /// Check if a buffer index is the left channel of a stereo pair
    [[nodiscard]] bool is_stereo_buffer(std::uint16_t buffer) const;

    /// Get stereo buffer pair for a node (must check is_stereo first)
    [[nodiscard]] StereoBuffers get_stereo_buffers(NodeIndex node) const;

    /// Get stereo buffer pair by left buffer index
    [[nodiscard]] StereoBuffers get_stereo_buffers_by_buffer(std::uint16_t buffer) const;

    // ============================================================================
    // Multi-buffer compatibility helpers (backed by node_types_)
    // ============================================================================

    /// Check if a node produces multiple buffers (Array type or stereo fallback)
    [[nodiscard]] bool is_multi_buffer(NodeIndex node) const;

    /// Get all buffers produced by a multi-buffer node
    [[nodiscard]] std::vector<std::uint16_t> get_multi_buffers(NodeIndex node) const;

    /// Apply a lambda expression to a single buffer value
    /// Creates a temporary scope, binds the parameter, generates body code
    /// @param lambda_node The Closure node containing parameter and body
    /// @param arg_buf The buffer to bind to the lambda parameter
    /// @return Output buffer from the lambda body
    std::uint16_t apply_lambda(NodeIndex lambda_node, std::uint16_t arg_buf);

    /// Resolve a function argument (can be inline lambda, lambda variable, or fn name)
    /// @param func_node The node that should resolve to a function
    /// @return FunctionRef if valid, nullopt otherwise
    std::optional<FunctionRef> resolve_function_arg(NodeIndex func_node);

    /// If `node` is an Identifier bound to a literal function-parameter argument
    /// (recorded in `param_literals_` during a user-function call), return the
    /// caller's literal node; otherwise return `node` unchanged. Lets builtins
    /// that need compile-time-constant args (e.g. linspace) see literals passed
    /// through a stdlib/user function parameter.
    NodeIndex resolve_param_literal(NodeIndex node) const;

    /// Apply a resolved function reference with captures.
    /// Generalized N-arity helper: binds `arg_bufs[i]` to `ref.params[i].name`
    /// for each supplied argument. Errors (E132) if the closure declares fewer
    /// parameters than arguments supplied, or if arity exceeds 32.
    /// @param ref The function reference containing closure/body info and captures
    /// @param arg_bufs The buffers to bind to the function's leading parameters
    /// @param loc Source location for error reporting
    /// @return Output buffer from the function body
    std::uint16_t apply_function_ref(const FunctionRef& ref,
                                     std::span<const std::uint16_t> arg_bufs,
                                     SourceLocation loc);

    /// Handle map(array, fn) call - apply function to each element
    TypedValue handle_map_call(NodeIndex node, const Node& n);

    /// Handle sum(array) call - reduce array by addition
    TypedValue handle_sum_call(NodeIndex node, const Node& n);

    /// Handle reduce(array, fn, init) call - reduce array with binary function
    TypedValue handle_reduce_call(NodeIndex node, const Node& n);

    /// Handle zipWith(a, b, fn) call - combine arrays element-wise with function
    TypedValue handle_zipWith_call(NodeIndex node, const Node& n);

    /// Handle zip(a, b) call - interleave two arrays
    TypedValue handle_zip_call(NodeIndex node, const Node& n);

    /// Handle take(n, array) call - take first n elements
    TypedValue handle_take_call(NodeIndex node, const Node& n);

    /// Handle drop(n, array) call - drop first n elements
    TypedValue handle_drop_call(NodeIndex node, const Node& n);

    /// Handle reverse(array) call - reverse array order
    TypedValue handle_reverse_call(NodeIndex node, const Node& n);

    /// Handle range(start, end) call - generate integer array
    TypedValue handle_range_call(NodeIndex node, const Node& n);

    /// Handle repeat(value, n) call - repeat value n times
    TypedValue handle_repeat_call(NodeIndex node, const Node& n);

    /// Handle spread(n, source) call - force specific voice count
    TypedValue handle_spread_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Array reduction operations
    // ============================================================================

    /// Handle mean(array) call - average of elements
    TypedValue handle_mean_call(NodeIndex node, const Node& n);

    /// Handle min/max calls - either binary or reduction depending on args
    TypedValue handle_minmax_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Array transformation operations
    // ============================================================================

    /// Handle rotate(array, n) call - rotate elements by n positions
    TypedValue handle_rotate_call(NodeIndex node, const Node& n);

    /// Handle shuffle(array) call - deterministic random permutation
    TypedValue handle_shuffle_call(NodeIndex node, const Node& n);

    /// Handle sort(array) call - sort elements in ascending order
    TypedValue handle_sort_call(NodeIndex node, const Node& n);

    /// Handle normalize(array) call - scale to 0-1 range
    TypedValue handle_normalize_call(NodeIndex node, const Node& n);

    /// Handle scale(array, lo, hi) call - map to new range
    TypedValue handle_scale_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Array generation operations
    // ============================================================================

    /// Handle linspace(start, end, n) call - N evenly spaced values
    TypedValue handle_linspace_call(NodeIndex node, const Node& n);

    /// Handle random(n) call - N random values (deterministic by path)
    TypedValue handle_random_call(NodeIndex node, const Node& n);

    /// Handle harmonics(fundamental, n) call - harmonic series
    TypedValue handle_harmonics_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Binary operation broadcasting
    // ============================================================================

    /// Handle binary operations (add, sub, mul, div, pow) with array broadcasting
    TypedValue handle_binary_op_call(NodeIndex node, const Node& n);

    // ============================================================================
    // Record support
    // ============================================================================

    /// Handle record literal nodes - expand to multiple buffers
    TypedValue handle_record_literal(NodeIndex node, const Node& n);

    /// Handle field access nodes - resolve to correct field buffer
    TypedValue handle_field_access(NodeIndex node, const Node& n);

    /// Phase 4b (records-system-unification): write sugar for record-valued
    /// state cells. `receiver.field = value` desugars to
    /// `set(receiver, {..get(receiver), field: value})`, which (because every
    /// non-touched field would round-trip through load+store) the codegen
    /// optimizes to a single STATE_OP rate=2 against the per-field sub-cell.
    /// Same observable behaviour, fewer instructions.
    ///
    /// Receiver type rules:
    ///   - StateCell holding a Record → write sugar applies (one rate=2 emit).
    ///   - Scalar StateCell → E135 (use `set(cell, value)` for scalar cells).
    ///   - Pure value Record → E150, with hint to declare `state({...})`.
    ///   - FieldAccess receiver (i.e. `cell.outer.x = v`) → E204 nested-write.
    TypedValue handle_field_assignment(NodeIndex node, const Node& n);

    /// Handle access to a qualified symbol from a namespace import (e.g., f.lp)
    TypedValue handle_qualified_symbol_access(NodeIndex node, const Symbol& qsym, SourceLocation loc);

    /// Handle pipe binding nodes - create buffer alias for named binding
    TypedValue handle_pipe_binding(NodeIndex node, const Node& n);

    // ============================================================================
    // Polyphonic pattern error tracking
    // ============================================================================
    // Track polyphonic patterns (max_voices > 1) that are not consumed by poly().
    // At the end of generate(), any remaining entries emit E410 errors.
    struct PolyPatternInfo {
        SourceLocation location;
        std::uint8_t max_voices;
    };
    std::unordered_map<NodeIndex, PolyPatternInfo> polyphonic_pattern_nodes_;
};

} // namespace akkado
