#pragma once

#include "akkado/akkado.hpp"
#include "cedar/vm/instruction.hpp"
#include <vector>
#include <string>
#include <string_view>
#include <span>
#include <optional>
#include <cstdint>

namespace nkido {

// Mode of operation
enum class Mode {
    Play,      // Compile (if needed) and play audio
    Dump,      // Display bytecode in human-readable format
    Compile,   // Compile source to bytecode file
    Check,     // Syntax check only
    UI,        // Interactive editor mode
    Render,    // Offline render to WAV (with optional voice trace)
    Serve      // Headless persistent mode for editor integration (JSON-over-stdio)
};

// Input type
enum class InputType {
    Stdin,        // Read from stdin
    SourceFile,   // .akkado file
    BytecodeFile, // .cedar file
    InlineSource  // Source string via --source argument
};

// Command-line options
struct Options {
    Mode mode = Mode::Play;
    InputType input_type = InputType::SourceFile;

    std::string input;                      // Input file path or source string
    std::optional<std::string> output_file; // For compile mode

    // Audio settings
    std::uint32_t sample_rate = 48000;
    std::uint32_t buffer_size = 128;

    // Output options
    bool dump_bytecode = false;  // Show bytecode before playing
    bool json_output = false;    // JSON format for errors
    bool verbose = false;        // Show compilation stats

    // Render mode options
    float render_seconds = 4.0f;                  // Duration for render mode
    std::optional<float> render_bpm;              // CLI --bpm override (renders honor patch BPM otherwise)
    std::optional<std::string> trace_poly_file;   // Optional path for poly state JSONL trace

    // Audio input options (Play/UI modes)
    bool list_devices = false;                    // Print capture devices and exit
    std::optional<std::string> input_device;      // Capture device name (nullopt = default)

    // MIDI input options (Play/UI/Serve modes; PRD docs/prd-midi-input.md §4.9)
    bool list_midi_devices = false;               // Print MIDI input ports and exit
    std::optional<std::string> midi_device;       // Default-device substring for bare midi(); nullopt = first available

    // URI-keyed asset flags (URI Resolver PRD §4.5). Each accepts any
    // scheme the resolver recognises (file://, http(s)://, github:,
    // bundled://, file path / bare path → file://).
    std::vector<std::string> bank_uris;           // Sample-bank manifests (strudel.json)
    std::vector<std::string> soundfont_uris;      // SoundFont (SF2/SF3) URIs
    std::vector<std::string> sample_uris;         // Single-sample URIs ("name=uri" or just "uri")

    // Built-in default sample kit (matches the web UI's DEFAULT_DRUM_KIT). When
    // false, prepare_program_assets() appends the discoverable bpb_808_clean
    // kit as a low-priority default bank so bare names like `bd`/`hh`/`sd`
    // resolve without an explicit --bank flag. Set true to suppress.
    bool no_default_bank = false;

    // Check if input needs compilation
    [[nodiscard]] bool needs_compilation() const {
        return input_type == InputType::SourceFile ||
               input_type == InputType::InlineSource ||
               input_type == InputType::Stdin;  // Stdin may need compilation
    }
};

// Compilation/load statistics
struct LoadStats {
    std::size_t source_bytes = 0;
    std::size_t instruction_count = 0;
    float compile_time_ms = 0.0f;
};

// Result of loading bytecode
struct LoadResult {
    bool success = false;
    std::vector<cedar::Instruction> instructions;
    std::string error_message;
    std::optional<LoadStats> stats;

    // Populated when input came from source (file / inline / stdin-as-source).
    // Empty for .cedar precompiled bytecode — that format does not carry the
    // RequiredSample / state-init metadata, so callers must skip asset
    // resolution for those inputs.
    std::optional<akkado::CompileResult> compile_result;
};

// Load bytecode based on options
// - SourceFile: compiles .akkado file
// - BytecodeFile: loads .cedar file directly
// - InlineSource: compiles source string
// - Stdin: reads from stdin, auto-detects format
LoadResult load_bytecode(const Options& opts);

// Read bytecode from file (binary .cedar format)
LoadResult read_bytecode_file(const std::string& path);

// Read from stdin and auto-detect format
LoadResult read_from_stdin();

// Compile source and return bytecode
LoadResult compile_source(std::string_view source, std::string_view filename);

// Compile source file
LoadResult compile_file(const std::string& path);

// Detect input type from file extension
InputType detect_input_type(const std::string& input);

// Check if data looks like bytecode (vs source code)
bool looks_like_bytecode(const std::vector<std::uint8_t>& data);

// Write bytecode to file
bool write_bytecode_file(const std::string& path,
                         std::span<const cedar::Instruction> instructions);

}  // namespace nkido
