// External pattern I/O — midi(), soundfont(), wavetable + input sources.
// Extracted verbatim from patterns.cpp (prd-codegen-sprawl-cleanup Phase 3).

#include "akkado/codegen.hpp"
#include "akkado/builtins.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/codegen/instruction_builder.hpp"
#include "akkado/codegen/state_init_builder.hpp"
#include "akkado/codegen/options.hpp"
#include "akkado/compile_context.hpp"
#include "akkado/chord_parser.hpp"
#include "akkado/pattern_eval.hpp"
#include "akkado/mini_parser.hpp"
#include "akkado/pattern_debug.hpp"
#include "akkado/tuning.hpp"
#include "akkado/voicing.hpp"
#include <cedar/opcodes/sequence.hpp>
#include <cedar/opcodes/event_transform_encoding.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace akkado {

using codegen::get_pattern_arg;
using codegen::get_number_arg;
using codegen::get_string_arg;

TypedValue CodeGenerator::handle_soundfont_call(NodeIndex node, const Node& n) {
    // soundfont(pattern, "file.sf2", preset) - SoundFont playback
    // The pattern provides gate, freq, velocity signals via polyphonic fields.
    // The file is a string literal (SF2 filename) resolved at compile time.
    // The preset is a number literal (index into the SF2's preset list).
    //
    // Usage: pat("c4 e4 g4") |> soundfont(%, "piano.sf2", 0) |> out(%, %)

    // Arg 0: pattern input (from pipe via %)
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "soundfont() requires a pattern as first argument "
              "(e.g., pat(\"c4 e4 g4\") |> soundfont(%, \"piano.sf2\", 0))", n.location);
        return TypedValue::void_val();
    }

    // Arg 1: filename (string literal)
    auto filename = get_string_arg(*ast_, n, 1);
    if (!filename.has_value()) {
        error("E131", "soundfont() requires a filename string as second argument "
              "(e.g., \"piano.sf2\")", n.location);
        return TypedValue::void_val();
    }

    // Arg 2: preset index (number literal)
    auto preset_opt = get_number_arg(*ast_, n, 2);
    if (!preset_opt.has_value()) {
        error("E132", "soundfont() requires a preset number as third argument", n.location);
        return TypedValue::void_val();
    }
    int preset_index = static_cast<int>(*preset_opt);

    // Visit pattern argument — this compiles the pattern and creates typed value
    auto pattern_tv = visit(pattern_arg);
    if (pattern_tv.buffer == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    // PRD prd-midi-input §7.1: detect runtime event source upstream (today:
    // midi(); future: anything else that sets is_runtime_event_source on
    // PatternPayload). Phase 5 Commit I removed the legacy EventSourcePayload
    // path — every runtime event stream now rides on PatternPayload with the
    // flag set.
    const bool upstream_is_event_source =
        pattern_tv.pattern && pattern_tv.pattern->is_runtime_event_source;
    const std::uint32_t upstream_seq_state_id = upstream_is_event_source
        ? pattern_tv.pattern->state_id : 0u;

    // Get pattern's fields (gate, freq, vel) from PatternPayload — only the
    // legacy buffer-driven path needs them.
    std::uint16_t gate_buf = 0xFFFF;
    std::uint16_t freq_buf = 0xFFFF;
    std::uint16_t vel_buf  = 0xFFFF;
    std::uint16_t trig_buf = 0xFFFF;
    std::vector<std::uint16_t> freq_per_voice;

    if (!upstream_is_event_source) {
        if (!pattern_tv.pattern) {
            error("E133", "soundfont() first argument must be a pattern that produces "
                  "gate/freq/vel fields", n.location);
            return TypedValue::void_val();
        }

        const auto& pat_fields = pattern_tv.pattern->fields;
        gate_buf = pat_fields[PatternPayload::GATE];
        freq_buf = pat_fields[PatternPayload::FREQ];
        vel_buf  = pat_fields[PatternPayload::VEL];
        trig_buf = pat_fields[PatternPayload::TRIG];

        if (gate_buf == 0xFFFF || freq_buf == 0xFFFF || vel_buf == 0xFFFF) {
            error("E133", "soundfont() pattern is missing required fields (gate, freq, vel)",
                  n.location);
            return TypedValue::void_val();
        }

        const auto& voice_freqs = pattern_tv.pattern->voice_freqs;
        if (voice_freqs.empty()) {
            freq_per_voice.push_back(freq_buf);
        } else {
            freq_per_voice = voice_freqs;
        }
    }

    // soundfont natively dispatches chord polyphony: each chord voice goes to
    // its own SoundFontVoiceState (with its own internal 32-voice allocator),
    // and the per-voice outputs are summed. Clear the E410 tracking entry —
    // poly() is not required for soundfont.
    polyphonic_pattern_nodes_.erase(pattern_arg);

    // Find or assign SF2 slot index (deduplicate by filename)
    std::uint8_t sf_slot = 0;
    bool found_slot = false;
    for (std::size_t i = 0; i < required_soundfonts_.size(); i++) {
        if (required_soundfonts_[i].filename == *filename) {
            sf_slot = static_cast<std::uint8_t>(i);
            found_slot = true;
            break;
        }
    }
    if (!found_slot) {
        sf_slot = static_cast<std::uint8_t>(required_soundfonts_.size());
        required_soundfonts_.push_back(RequiredSoundFont{*filename, preset_index});
    }

    std::uint32_t sf_count = call_counters_["soundfont"]++;
    push_path("soundfont#" + std::to_string(sf_count));

    // PRD prd-midi-input §7.1: MIDI-upstream (event-driven) path. One
    // SOUNDFONT_VOICE with unwired inputs; the opcode resolves events from
    // seq_state_id and uses its internal 32-voice allocator for chord
    // polyphony driven by live note-ons.
    std::vector<std::uint16_t> per_voice_outs;
    if (upstream_is_event_source) {
        std::uint32_t state_id = compute_state_id();

        // inputs[0] = 0xFFFF signals event-driven mode to op_soundfont_voice.
        const std::uint16_t out_buf =
            codegen::InstructionBuilder(cedar::Opcode::SOUNDFONT_VOICE)
                .state_id(state_id)
                .rate(sf_slot)
                .emit(*this, n.location);
        if (out_buf == BufferAllocator::BUFFER_UNUSED) {
            pop_path();
            return TypedValue::void_val();
        }

        // Tell the host to seed the SoundFontVoiceState with the upstream
        // state_id and the preset index before audio starts.
        codegen::StateInitBuilder::soundfont_events(state_id)
            .sf_seq_state_id(upstream_seq_state_id)
            .sf_preset_idx(preset_index)
            .publish(*this);

        per_voice_outs.push_back(out_buf);
    } else {
        // Legacy buffer-driven path: emit a constant preset buffer and one
        // SOUNDFONT_VOICE per chord voice, each with its own state_id.
        std::uint16_t preset_buf = emit_push_const(static_cast<float>(preset_index));
        if (preset_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::void_val();
        }

        per_voice_outs.reserve(freq_per_voice.size());
        for (std::size_t v = 0; v < freq_per_voice.size(); ++v) {
            push_path("voice#" + std::to_string(v));
            std::uint32_t state_id = compute_state_id();

            const std::uint16_t out_buf =
                codegen::InstructionBuilder(cedar::Opcode::SOUNDFONT_VOICE)
                    .inputs({gate_buf, freq_per_voice[v], vel_buf,
                             preset_buf, trig_buf})
                    .state_id(state_id)
                    .rate(sf_slot)
                    .emit(*this, n.location);
            if (out_buf == BufferAllocator::BUFFER_UNUSED) {
                pop_path();
                pop_path();
                return TypedValue::void_val();
            }

            per_voice_outs.push_back(out_buf);
            pop_path();
        }
    }

    // Sum per-voice outputs into one buffer. Single voice = no sum needed.
    std::uint16_t mixed = per_voice_outs[0];
    for (std::size_t v = 1; v < per_voice_outs.size(); ++v) {
        std::uint16_t sum_buf = alloc_buffer(n.location);
        if (sum_buf == BufferAllocator::BUFFER_UNUSED) {
            pop_path();
            return TypedValue::void_val();
        }
        emit(cedar::Instruction::make_binary(cedar::Opcode::ADD, sum_buf,
                                              mixed, per_voice_outs[v]));
        mixed = sum_buf;
    }

    // Chord-polyphony RMS normalization: scale the summed N voices by
    // 1/sqrt(N) so an N-note chord has the same perceived loudness as a
    // single note. Without this, e.g. `c"Am" |> soundfont(@, "gm", 0)` would
    // sum 3 full-amplitude voices and clip in the WAV output stage.
    const std::size_t n_voices = per_voice_outs.size();
    if (n_voices > 1) {
        const float scale = 1.0f / std::sqrt(static_cast<float>(n_voices));
        std::uint16_t scale_buf = emit_push_const(scale);
        if (scale_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::void_val();
        }
        std::uint16_t scaled_buf = alloc_buffer(n.location);
        if (scaled_buf == BufferAllocator::BUFFER_UNUSED) {
            pop_path();
            return TypedValue::void_val();
        }
        emit(cedar::Instruction::make_binary(cedar::Opcode::MUL, scaled_buf,
                                              mixed, scale_buf));
        mixed = scaled_buf;
    }

    pop_path();
    return cache_and_return(node, TypedValue::signal(mixed));
}

// sf_voice(file, preset, freq, gate, vel) — single-voice SoundFont player.
//
// Unlike soundfont() (which owns its own 32-voice pool and consumes a
// pattern), sf_voice is an ordinary instrument expression: it takes three
// signal arguments and produces a stereo signal. Used as the instrument body
// of poly(), it gives soundfont playback the same voice-management path as
// every other instrument.
//
//   n"c4 e4 g4" |> poly(@, (f,g,v) -> sf_voice("piano.sf2", 0, f, g, v)) |> out(@, @)
//
// `file` is a string literal — either a path or an alias registered via the
// $soundfont_alias directive. `preset` is a number literal (SF2 preset
// index). freq/gate/vel are arbitrary signal expressions.
TypedValue CodeGenerator::handle_sf_voice_call(NodeIndex node, const Node& n) {
    // Arg 0: file (string literal — path or $soundfont_alias name)
    auto filename = get_string_arg(*ast_, n, 0);
    if (!filename.has_value()) {
        error("E520", "sf_voice() requires a filename string as first argument "
              "(a path or a $soundfont_alias name)", n.location);
        return TypedValue::error_val();
    }

    // Arg 1: preset index (number literal)
    auto preset_opt = get_number_arg(*ast_, n, 1);
    if (!preset_opt.has_value()) {
        error("E521", "sf_voice() requires a preset number as second argument",
              n.location);
        return TypedValue::error_val();
    }
    int preset_index = static_cast<int>(*preset_opt);

    // Args 2/3/4: freq, gate, vel signal expressions.
    NodeIndex freq_arg = get_pattern_arg(*ast_, n, 2);
    NodeIndex gate_arg = get_pattern_arg(*ast_, n, 3);
    NodeIndex vel_arg  = get_pattern_arg(*ast_, n, 4);
    if (freq_arg == NULL_NODE || gate_arg == NULL_NODE || vel_arg == NULL_NODE) {
        error("E522", "sf_voice() requires freq, gate and vel signal arguments "
              "(sf_voice(file, preset, freq, gate, vel))", n.location);
        return TypedValue::error_val();
    }

    TypedValue freq_tv = visit(freq_arg);
    TypedValue gate_tv = visit(gate_arg);
    TypedValue vel_tv  = visit(vel_arg);
    if (freq_tv.buffer == BufferAllocator::BUFFER_UNUSED ||
        gate_tv.buffer == BufferAllocator::BUFFER_UNUSED ||
        vel_tv.buffer  == BufferAllocator::BUFFER_UNUSED) {
        error("E522", "sf_voice() freq/gate/vel arguments must be signals",
              n.location);
        return TypedValue::error_val();
    }

    // Resolve $soundfont_alias entries at compile time (directive precedence,
    // PRD §3.5). Depth-limited so a cyclic alias chain can't hang codegen;
    // unresolved names pass through to the host, which applies runtime
    // --soundfont-alias config and the built-in alias table.
    std::string resolved_file = *filename;
    for (int depth = 0; depth < 8; ++depth) {
        auto it = soundfont_aliases_.find(resolved_file);
        if (it == soundfont_aliases_.end()) break;
        resolved_file = it->second;
    }

    // Find or assign SF2 slot index (deduplicate by resolved filename). The
    // runtime registry loads required_soundfonts_ in source order, so slot N
    // here matches registry slot N (see program_loader.cpp).
    std::uint8_t sf_slot = 0;
    bool found_slot = false;
    for (std::size_t i = 0; i < required_soundfonts_.size(); i++) {
        if (required_soundfonts_[i].filename == resolved_file) {
            sf_slot = static_cast<std::uint8_t>(i);
            found_slot = true;
            break;
        }
    }
    if (!found_slot) {
        sf_slot = static_cast<std::uint8_t>(required_soundfonts_.size());
        required_soundfonts_.push_back(RequiredSoundFont{resolved_file, preset_index});
    }

    std::uint32_t sf_count = call_counters_["sf_voice"]++;
    push_path("sf_voice#" + std::to_string(sf_count));

    // Preset index as a constant input buffer (read at inputs[3]).
    std::uint16_t preset_buf = emit_push_const(static_cast<float>(preset_index));
    if (preset_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::error_val();
    }

    // Allocate the adjacent stereo output pair (L = out_left, R = out_left+1).
    std::uint16_t out_left = alloc_buffer(n.location);
    if (out_left == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        return TypedValue::error_val();
    }
    std::uint16_t out_right = alloc_buffer(n.location);
    if (out_right == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        return TypedValue::error_val();
    }
    if (out_right != out_left + 1) {
        error("E166", "Internal error: stereo buffer allocation not adjacent",
              n.location);
        pop_path();
        return TypedValue::error_val();
    }

    codegen::InstructionBuilder(cedar::Opcode::SF_VOICE)
        .inputs({gate_tv.buffer, freq_tv.buffer, vel_tv.buffer, preset_buf})
        .output(out_left)
        .rate(sf_slot)
        .state_id(compute_state_id())
        .flags(static_cast<std::uint16_t>(
            cedar::InstructionFlag::STEREO_OUTPUT))
        .emit(*this);

    pop_path();

    register_stereo(node, out_left, out_right);
    return cache_and_return(node, TypedValue::stereo_signal(out_left, out_right));
}

// PRD prd-midi-input §4.7 + §7.5: midi() builtin. Emits one MIDI_QUERY with
// four output buffers (gate/freq/vel/trig) baked monophonically from the
// runtime event stream, and records a RequiredMidiSource that the host
// iterates to call vm.init_midi_queue_state after load.
//
// Returns a TypedValue::Pattern with `is_runtime_event_source = true`. Both
// shapes work downstream:
//   * `midi() as e |> sine(e.freq) |> @ * adsr(e.gate)` reads the
//     mono-baked buffers via pattern field access.
//   * `midi() |> poly(@, synth, 8)` and (Phase 7.1) `midi() |> soundfont(...)`
//     read the polyphonic OutputEvents via state_pool_.resolve_output_events.
// handle_poly_call still picks up state_id from pattern->state_id.
TypedValue CodeGenerator::handle_midi_call(NodeIndex node, const Node& n) {
    // §10 Q4 / Non-Goal: midi() top-level only in v1.
    if (user_function_depth_ > 0) {
        error("E412",
              "midi() may only be called at the top level — not inside fn bodies "
              "(per prd-midi-input §10).",
              n.location);
        return TypedValue::void_val();
    }

    // Walk children. Accept 0 or 1 record argument.
    NodeIndex arg = n.first_child;
    NodeIndex options_arg = NULL_NODE;
    std::size_t arg_count = 0;
    while (arg != NULL_NODE) {
        ++arg_count;
        if (arg_count == 1) options_arg = arg;
        arg = ast_->arena[arg].next_sibling;
    }
    if (arg_count > 1) {
        error("E400",
              "midi() takes at most one argument: an options record "
              "(e.g. midi({device: \"name\"}) or midi({file: \"song.mid\"})).",
              n.location);
        return TypedValue::void_val();
    }

    // Defaults
    cedar::MidiSourceKind kind = cedar::MidiSourceKind::DefaultDevice;
    std::string           name_or_path;
    std::uint8_t          channel_filter = 0;
    bool                  loop = false;
    cedar::MidiQueueState::TempoMode tempo_mode =
        cedar::MidiQueueState::TempoMode::Follow;

    if (options_arg != NULL_NODE) {
        // Enforce midi's `{Record}` param_type (PRD prd-builtin-overload-resolution
        // Goal 4 / §10). midi routes through LegacyHandler::Midi and never calls
        // resolve(), so without this guard a non-record options arg (e.g.
        // midi(440), midi("x")) would slip straight into extract_options, which
        // silently returns an empty payload for a non-RecordLit — opening the
        // default device with no diagnostic. Reject it like transport's E133.
        NodeIndex options_value = options_arg;
        if (ast_->arena[options_arg].type == NodeType::Argument) {
            options_value = ast_->arena[options_arg].first_child;
        }
        if (options_value == NULL_NODE ||
            ast_->arena[options_value].type != NodeType::RecordLit) {
            error("E425",
                  "midi() options must be a record literal "
                  "(e.g. midi({device: \"name\"}) or midi({file: \"song.mid\"})).",
                  n.location);
            return TypedValue::void_val();
        }

        // Look up the options schema declared on the builtin.
        const OptionSchema* schema_ptr = nullptr;
        if (const BuiltinInfo* info = lookup_builtin("midi")) {
            schema_ptr = info->find_option_schema(/*param_index=*/0);
        }
        static const OptionSchema empty_schema{};
        const OptionSchema& schema = schema_ptr ? *schema_ptr : empty_schema;

        codegen::OptionsPayload payload =
            codegen::extract_options(ast_->arena, options_arg, schema);

        auto device_opt = payload.get_string("device");
        auto file_opt   = payload.get_string("file");
        auto channel_opt = payload.get_number("channel");
        auto loop_opt   = payload.get_bool("loop");
        auto tempo_opt  = payload.get_string("tempo");

        const bool has_device = device_opt.has_value() && !device_opt->empty();
        const bool has_file   = file_opt.has_value()   && !file_opt->empty();

        if (has_device && has_file) {
            error("E411",
                  "midi(): 'file' and 'device' are mutually exclusive — "
                  "pick one source.",
                  n.location);
            return TypedValue::void_val();
        }

        if (has_file) {
            kind = cedar::MidiSourceKind::File;
            name_or_path.assign(file_opt->data(), file_opt->size());
        } else if (has_device) {
            kind = cedar::MidiSourceKind::NamedDevice;
            name_or_path.assign(device_opt->data(), device_opt->size());
        }

        if (channel_opt.has_value()) {
            double ch = *channel_opt;
            int chi = static_cast<int>(ch);
            if (chi < 0 || chi > 16 ||
                static_cast<double>(chi) != ch) {
                error("E414",
                      "midi(): 'channel' must be an integer in 0..16 "
                      "(0 = any channel, 1..16 = specific MIDI channel).",
                      n.location);
                return TypedValue::void_val();
            }
            channel_filter = static_cast<std::uint8_t>(chi);
        }

        if (loop_opt.has_value()) {
            loop = *loop_opt;
        }

        if (tempo_opt.has_value()) {
            if (*tempo_opt == "follow") {
                tempo_mode = cedar::MidiQueueState::TempoMode::Follow;
            } else if (*tempo_opt == "file") {
                tempo_mode = cedar::MidiQueueState::TempoMode::File;
            } else {
                error("E413",
                      "midi(): 'tempo' must be \"follow\" or \"file\" "
                      "(got \"" + std::string(*tempo_opt) + "\").",
                      n.location);
                return TypedValue::void_val();
            }
        }
    }

    // Unique semantic-id path per call site so hot-swap state preservation
    // matches the right ring across recompiles.
    std::uint32_t midi_count = call_counters_["midi"]++;
    push_path("midi#" + std::to_string(midi_count));
    std::uint32_t state_id = compute_state_id();

    // PRD §7.5: allocate per-block mono buffers. op_midi_query reads these
    // indices off the instruction (inst.inputs[0..3]) and fills them via
    // fill_mono_buffers. All four allocate together — if any fails, signal
    // E101 and skip emission. (Buffer pool exhaustion is extremely rare
    // for a top-level midi() call but we report it cleanly to match the
    // pattern in handle_poly_call.)
    std::uint16_t gate_buf = buffers_.allocate();
    std::uint16_t freq_buf = buffers_.allocate();
    std::uint16_t vel_buf  = buffers_.allocate();
    std::uint16_t trig_buf = buffers_.allocate();
    if (gate_buf == BufferAllocator::BUFFER_UNUSED ||
        freq_buf == BufferAllocator::BUFFER_UNUSED ||
        vel_buf  == BufferAllocator::BUFFER_UNUSED ||
        trig_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted in midi()", n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // Emit one MIDI_QUERY. The four mono buffers ride in inputs[0..3]
    // (treated as output destinations by op_midi_query). Slot 4 is unused.
    // POLY / SOUNDFONT continue to read MidiQueueState.output via
    // state_pool_.resolve_output_events for full polyphonic event access.
    codegen::InstructionBuilder(cedar::Opcode::MIDI_QUERY)
        .output(0xFFFF)
        .inputs({gate_buf, freq_buf, vel_buf, trig_buf})
        .state_id(state_id)
        .emit(*this);

    // Publish host-facing config. Duplicates by name are NOT collapsed —
    // each call gets its own state_id and its own ring (PRD §3.4).
    required_midi_sources_.push_back(RequiredMidiSource{
        /*state_id=*/state_id,
        /*kind=*/kind,
        /*name_or_path=*/std::move(name_or_path),
        /*channel_filter=*/channel_filter,
        /*loop=*/loop,
        /*tempo_mode=*/tempo_mode,
    });

    pop_path();

    // PRD §7.5: return a Pattern-shaped TypedValue so `as e |> ... e.freq`
    // field access goes through the existing pattern_field() dispatch.
    // is_runtime_event_source signals to consumers (Phase 7.1 soundfont,
    // future migrations) that OutputEvents is the source of truth.
    auto payload = std::make_shared<PatternPayload>();
    payload->state_id                 = state_id;
    payload->cycle_length             = 1.0f;
    payload->max_voices               = 1;
    payload->is_runtime_event_source  = true;
    payload->fields[PatternPayload::FREQ] = freq_buf;
    payload->fields[PatternPayload::VEL]  = vel_buf;
    payload->fields[PatternPayload::TRIG] = trig_buf;
    payload->fields[PatternPayload::GATE] = gate_buf;
    return cache_and_return(node, TypedValue::make_pattern(payload, freq_buf));
}

// PRD prd-midi-input §4.8: midi_cc("name", {...}) — compile-time only.
// Records a RequiredMidiCcRoute entry in `required_midi_cc_routes_`. The
// host MIDI callback walks this list and calls vm.set_param() for matching
// events. No instruction is emitted.
TypedValue CodeGenerator::handle_midi_cc_call(NodeIndex node, const Node& n) {
    // §10 Q4 / Non-Goal: midi_cc() top-level only in v1 (same as midi()).
    if (user_function_depth_ > 0) {
        error("E412",
              "midi_cc() may only be called at the top level — not inside fn bodies "
              "(per prd-midi-input §10).",
              n.location);
        return TypedValue::void_val();
    }

    // Walk children. Require name (string literal) + options record.
    NodeIndex name_arg = NULL_NODE;
    NodeIndex options_arg = NULL_NODE;
    std::size_t arg_count = 0;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        ++arg_count;
        if (arg_count == 1) name_arg = arg;
        else if (arg_count == 2) options_arg = arg;
        arg = ast_->arena[arg].next_sibling;
    }
    if (arg_count != 2) {
        error("E400",
              "midi_cc() takes two arguments: a param name string and an options "
              "record (e.g. midi_cc(\"cutoff\", {cc: 74})).",
              n.location);
        return TypedValue::void_val();
    }

    auto name_opt = get_string_arg(*ast_, n, 0);
    if (!name_opt.has_value() || name_opt->empty()) {
        error("E400",
              "midi_cc(): first argument must be a non-empty string literal "
              "naming the target param() slot.",
              n.location);
        return TypedValue::void_val();
    }
    std::string param_name = *name_opt;
    (void)name_arg;  // suppress unused-warning; the walk above is only for arity

    // Look up the options schema declared on the builtin.
    const OptionSchema* schema_ptr = nullptr;
    if (const BuiltinInfo* info = lookup_builtin("midi_cc")) {
        schema_ptr = info->find_option_schema(/*param_index=*/1);
    }
    static const OptionSchema empty_schema{};
    const OptionSchema& schema = schema_ptr ? *schema_ptr : empty_schema;

    codegen::OptionsPayload payload =
        codegen::extract_options(ast_->arena, options_arg, schema);

    auto cc_opt      = payload.get_number("cc");
    auto channel_opt = payload.get_number("channel");
    auto pb_opt      = payload.get_bool("pb");
    auto at_opt      = payload.get_bool("at");
    auto min_opt     = payload.get_number("min");
    auto max_opt     = payload.get_number("max");
    auto slew_opt    = payload.get_number("slew");

    // Default-cc sentinel is -128 in the schema; treat any negative value as "unset".
    const bool cc_present = cc_opt.has_value() && *cc_opt >= 0.0;
    const bool pb_set     = pb_opt.value_or(false);
    const bool at_set     = at_opt.value_or(false);

    const int set_count = (cc_present ? 1 : 0) + (pb_set ? 1 : 0) + (at_set ? 1 : 0);
    if (set_count == 0) {
        error("E421",
              "midi_cc(): one of cc:, pb:, or at: must be set "
              "(e.g. midi_cc(\"name\", {cc: 74}), {pb: true}, or {at: true}).",
              n.location);
        return TypedValue::void_val();
    }
    if (set_count > 1) {
        error("E420",
              "midi_cc(): exactly one of cc:, pb:, or at: may be set — "
              "they are mutually exclusive.",
              n.location);
        return TypedValue::void_val();
    }

    std::int16_t cc_num = 0;
    if (cc_present) {
        double cv = *cc_opt;
        int cci = static_cast<int>(cv);
        if (cci < 0 || cci > 127 || static_cast<double>(cci) != cv) {
            error("E422",
                  "midi_cc(): 'cc' must be an integer in 0..127.",
                  n.location);
            return TypedValue::void_val();
        }
        cc_num = static_cast<std::int16_t>(cci);
    } else if (pb_set) {
        cc_num = -1;
    } else {
        cc_num = -2;
    }

    std::uint8_t channel_filter = 0;
    if (channel_opt.has_value()) {
        double ch = *channel_opt;
        int chi = static_cast<int>(ch);
        if (chi < 0 || chi > 16 || static_cast<double>(chi) != ch) {
            error("E414",
                  "midi_cc(): 'channel' must be an integer in 0..16 "
                  "(0 = any channel, 1..16 = specific MIDI channel).",
                  n.location);
            return TypedValue::void_val();
        }
        channel_filter = static_cast<std::uint8_t>(chi);
    }

    // PRD §4.8: default range is 0..1 for cc/at and -1..+1 for pb.
    double range_min = pb_set ? -1.0 : 0.0;
    double range_max = 1.0;
    if (min_opt.has_value()) range_min = *min_opt;
    if (max_opt.has_value()) range_max = *max_opt;
    float scale = static_cast<float>(range_max - range_min);
    float bias  = static_cast<float>(range_min);

    float slew_ms = 5.0f;
    if (slew_opt.has_value()) {
        double s = *slew_opt;
        if (s < 0.0) s = 0.0;
        slew_ms = static_cast<float>(s);
    }

    required_midi_cc_routes_.push_back(RequiredMidiCcRoute{
        /*param_name=*/std::move(param_name),
        /*cc_num=*/cc_num,
        /*channel_filter=*/channel_filter,
        /*scale=*/scale,
        /*bias=*/bias,
        /*slew_ms=*/slew_ms,
    });

    // No instruction emitted; the directive is compile-time metadata only.
    return cache_and_return(node, TypedValue::void_val());
}

TypedValue CodeGenerator::handle_input_call(NodeIndex node, const Node& n) {
    // in() — defaults to host UI source.
    // in("mic" | "tab" | "file:NAME") — overrides the source for this compile.
    //
    // Emits a single INPUT instruction that copies ctx.input_left/right into
    // an adjacent buffer pair. Result is a Stereo signal so downstream mono
    // DSP auto-lifts via the universal stereo signal semantics.

    // Validate argument count: 0 or 1.
    std::size_t arg_count = 0;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        ++arg_count;
        arg = ast_->arena[arg].next_sibling;
    }
    if (arg_count > 1) {
        error("E190", "in() takes 0 or 1 arguments (got " +
              std::to_string(arg_count) + ")", n.location);
        return TypedValue::error_val();
    }

    // Optional source string literal (e.g. "mic", "tab", "file:voice.wav").
    std::string source_str;
    if (arg_count == 1) {
        auto src = get_string_arg(*ast_, n, 0);
        if (!src.has_value()) {
            error("E191", "in() argument must be a string literal "
                  "(\"mic\", \"tab\", or \"file:NAME\")", n.location);
            return TypedValue::error_val();
        }
        const std::string& s = *src;
        // Validate the source grammar — fail loud on typos so the user gets a
        // clear diagnostic rather than silent fallback to default.
        bool ok = (s == "mic" || s == "tab")
               || (s.rfind("file:", 0) == 0 && s.size() > 5);
        if (!ok) {
            error("E192", "in() unknown source \"" + s +
                  "\". Expected \"mic\", \"tab\", or \"file:NAME\".",
                  n.location);
            return TypedValue::error_val();
        }
        source_str = s;
    }
    required_input_sources_.push_back(source_str);

    // Allocate adjacent L/R buffer pair (linear allocator guarantees right=left+1).
    std::uint16_t left_buf  = alloc_buffer(n.location);
    std::uint16_t right_buf = alloc_buffer(n.location);
    if (left_buf == BufferAllocator::BUFFER_UNUSED ||
        right_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::error_val();
    }

    // Sanity-check the adjacent-buffer invariant — every other stereo opcode
    // depends on it (PAN, WIDTH, INPUT all read out_buffer+1 for the right slot).
    if (right_buf != left_buf + 1) {
        error("E101", "Internal: input() failed to allocate adjacent stereo "
              "buffer pair", n.location);
        return TypedValue::error_val();
    }

    // Stateless (state_id 0).
    codegen::InstructionBuilder(cedar::Opcode::INPUT)
        .output(left_buf)
        .emit(*this);

    register_stereo(node, left_buf, right_buf);
    return cache_and_return(node, TypedValue::stereo_signal(left_buf, right_buf));
}

TypedValue CodeGenerator::handle_wt_load_call(NodeIndex node, const Node& n) {
    // wt_load("name", "path") — register a wavetable bank for the host to
    // load. Compile-time directive only (emits no audio-time instruction).
    // Bank IDs are assigned in source order: first wt_load = 0, second = 1,
    // etc. The runtime registry MUST be cleared before loading these in
    // the same order so the IDs match.
    (void)node;

    std::size_t arg_count = 0;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        ++arg_count;
        arg = ast_->arena[arg].next_sibling;
    }
    if (arg_count != 2) {
        error("E193",
              "wt_load() takes 2 arguments (name, path) — got "
              + std::to_string(arg_count),
              n.location);
        return TypedValue::error_val();
    }

    auto name_opt = get_string_arg(*ast_, n, 0);
    if (!name_opt.has_value()) {
        error("E194",
              "wt_load() first argument must be a string literal "
              "(bank name, e.g. \"Basic Shapes\")",
              n.location);
        return TypedValue::error_val();
    }
    auto path_opt = get_string_arg(*ast_, n, 1);
    if (!path_opt.has_value()) {
        error("E195",
              "wt_load() second argument must be a string literal "
              "(file path, e.g. \"wavetables/basic.wav\")",
              n.location);
        return TypedValue::error_val();
    }

    // Dedup on name. If the user repeats wt_load("foo", ...) we keep the
    // first declaration (and reuse its ID for subsequent smooch references).
    // A path mismatch is a warning-worthy event but for now we accept the
    // first entry silently; multi-source-file imports are the typical
    // reason for the duplicate.
    for (const auto& w : required_wavetables_) {
        if (w.name == *name_opt) {
            return TypedValue::void_val();
        }
    }

    if (required_wavetables_.size() >= 64) {
        error("E196",
              "wt_load: too many wavetable banks (max 64 per program)",
              n.location);
        return TypedValue::error_val();
    }

    RequiredWavetable rw;
    rw.name = *name_opt;
    rw.path = *path_opt;
    rw.id   = static_cast<int>(required_wavetables_.size());
    required_wavetables_.push_back(std::move(rw));
    return TypedValue::void_val();
}

TypedValue CodeGenerator::handle_samples_call(NodeIndex node, const Node& n) {
    // samples("uri") — declare a sample-bank URI for the host to load.
    // Compile-time directive only (emits no audio-time instruction). The
    // argument must be a string literal; any URI scheme is accepted (the
    // host's URI resolver dispatches based on the scheme prefix).
    (void)node;

    std::size_t arg_count = 0;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        ++arg_count;
        arg = ast_->arena[arg].next_sibling;
    }
    if (arg_count != 1) {
        error("E230",
              "samples() takes 1 argument (uri) — got "
              + std::to_string(arg_count),
              n.location);
        return TypedValue::error_val();
    }

    auto uri_opt = get_string_arg(*ast_, n, 0);
    if (!uri_opt.has_value()) {
        error("E231",
              "samples() argument must be a string literal "
              "(URI, e.g. \"github:tidalcycles/Dirt-Samples\")",
              n.location);
        return TypedValue::error_val();
    }

    if (uri_opt->empty()) {
        error("E232",
              "samples() URI cannot be empty",
              n.location);
        return TypedValue::error_val();
    }

    // Dedup: same URI declared twice in source is a no-op.
    for (const auto& u : required_uris_) {
        if (u.kind == UriKind::SampleBank && u.uri == *uri_opt) {
            return TypedValue::void_val();
        }
    }

    UriRequest req;
    req.uri  = *uri_opt;
    req.kind = UriKind::SampleBank;
    required_uris_.push_back(std::move(req));
    return TypedValue::void_val();
}

TypedValue CodeGenerator::handle_smooch_call(NodeIndex node, const Node& n) {
    // smooch("bank_name", freq, phase?, tablePos?) — wavetable oscillator.
    // First arg is a string literal naming a bank previously declared via
    // wt_load(). The bank ID is resolved at compile time and packed into
    // inst.rate. Remaining args are audio-rate signals (phase and tablePos
    // default to BUFFER_UNUSED → 0 via get_input_or_zero in the opcode).

    // Count args
    std::size_t arg_count = 0;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        ++arg_count;
        arg = ast_->arena[arg].next_sibling;
    }
    if (arg_count < 2 || arg_count > 4) {
        error("E197",
              "smooch() takes 2-4 arguments (\"bank\", freq, phase?, tablePos?) — got "
              + std::to_string(arg_count),
              n.location);
        return TypedValue::error_val();
    }

    auto bank_name = get_string_arg(*ast_, n, 0);
    if (!bank_name.has_value()) {
        error("E198",
              "smooch() first argument must be a string literal (bank name "
              "from a wt_load() call, e.g. smooch(\"morph\", freq))",
              n.location);
        return TypedValue::error_val();
    }

    // Resolve bank name → ID via the required_wavetables_ list. The user
    // must call wt_load("name", "path") earlier in source.
    int bank_id = -1;
    for (const auto& w : required_wavetables_) {
        if (w.name == *bank_name) {
            bank_id = w.id;
            break;
        }
    }
    if (bank_id < 0) {
        error("E199",
              "smooch(\"" + *bank_name + "\", ...) references an unknown wavetable. "
              "Add wt_load(\"" + *bank_name + "\", \"path/to/wavetable.wav\") "
              "before this call.",
              n.location);
        return TypedValue::error_val();
    }

    // Compile each signal arg via the generic visit() pipeline. Position 0
    // was the bank-name string and is skipped.
    auto compile_signal = [&](std::size_t arg_index, std::uint16_t& out_buf,
                                bool required) -> bool {
        NodeIndex sig_arg = arg_index < arg_count
                          ? get_pattern_arg(*ast_, n, arg_index)
                          : NULL_NODE;
        if (sig_arg == NULL_NODE) {
            if (required) {
                error("E197",
                      "smooch() argument " + std::to_string(arg_index)
                      + " is required",
                      n.location);
                return false;
            }
            out_buf = BufferAllocator::BUFFER_UNUSED;
            return true;
        }
        TypedValue tv = visit(sig_arg);
        if (tv.error) return false;
        if (tv.type != ValueType::Signal && tv.type != ValueType::Number) {
            error("E199",
                  "smooch() argument " + std::to_string(arg_index)
                  + " must be a signal or number, got "
                  + value_type_name(tv.type),
                  n.location);
            return false;
        }
        out_buf = tv.buffer;
        return true;
    };

    std::uint16_t freq_buf  = BufferAllocator::BUFFER_UNUSED;
    std::uint16_t phase_buf = BufferAllocator::BUFFER_UNUSED;
    std::uint16_t pos_buf   = BufferAllocator::BUFFER_UNUSED;
    if (!compile_signal(1, freq_buf,  /*required=*/true))  return TypedValue::error_val();
    if (!compile_signal(2, phase_buf, /*required=*/false)) return TypedValue::error_val();
    if (!compile_signal(3, pos_buf,   /*required=*/false)) return TypedValue::error_val();

    // Allocate the per-voice state ID using the standard semantic-path
    // hashing (matches the convention used by every other stateful opcode
    // — preserves phase across hot-swap when the same smooch call survives
    // a recompile).
    std::uint32_t smooch_count = call_counters_["smooch"]++;
    push_path("smooch#" + std::to_string(smooch_count));
    std::uint32_t state_id = compute_state_id();
    pop_path();

    const std::uint16_t out_buf =
        codegen::InstructionBuilder(cedar::Opcode::OSC_WAVETABLE)
            .inputs({freq_buf, phase_buf, pos_buf})
            .state_id(state_id)
            .rate(static_cast<std::uint8_t>(bank_id))
            .emit(*this, n.location);
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::error_val();
    }

    return cache_and_return(node, TypedValue::signal(out_buf));
}

} // namespace akkado
