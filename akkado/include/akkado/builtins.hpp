#pragma once

// The builtin registry: BUILTIN_FUNCTIONS / BUILTIN_ALIASES /
// BUILTIN_VARIABLES tables + lookup helpers. The BuiltinInfo struct and
// friends live in builtin_info.hpp so headers that only need the type
// (symbol_table.hpp, host_extensions.hpp) don't depend on this table —
// and so this header can include codegen.hpp: the table stores
// &CodeGenerator::... codegen_handler member pointers, which require the
// complete CodeGenerator class (PRD prd-codegen-sprawl-cleanup Phase 4).

#include "akkado/builtin_info.hpp"
#include "akkado/codegen.hpp"

#include <frozen/string.h>
#include <frozen/unordered_map.h>

namespace akkado {

/// PRD prd-codegen-sprawl-cleanup Phase 4: named member-pointer constants
/// for every builtin with custom codegen. CodeGenerator befriends this
/// struct, so the pointers can name private handler methods; the table
/// below references them via `.codegen_handler = BuiltinHandlers::...`.
/// Adding a custom-codegen builtin = one constant here + one field on its
/// BUILTIN_FUNCTIONS entry (the old 100-entry name-keyed dispatch map in
/// visit() is gone).
struct BuiltinHandlers {
    static constexpr CodegenHandler handle_add_voicings_call = &CodeGenerator::handle_add_voicings_call;
    static constexpr CodegenHandler handle_anchor_call = &CodeGenerator::handle_anchor_call;
    static constexpr CodegenHandler handle_bank_call = &CodeGenerator::handle_bank_call;
    static constexpr CodegenHandler handle_binary_call = &CodeGenerator::handle_binary_call;
    static constexpr CodegenHandler handle_binary_n_call = &CodeGenerator::handle_binary_n_call;
    static constexpr CodegenHandler handle_bus_call = &CodeGenerator::handle_bus_call;
    static constexpr CodegenHandler handle_button_call = &CodeGenerator::handle_button_call;
    static constexpr CodegenHandler handle_chord_call = &CodeGenerator::handle_chord_call;
    static constexpr CodegenHandler handle_compose_call = &CodeGenerator::handle_compose_call;
    static constexpr CodegenHandler handle_compress_call = &CodeGenerator::handle_compress_call;
    static constexpr CodegenHandler handle_drop_call = &CodeGenerator::handle_drop_call;
    static constexpr CodegenHandler handle_event_filter_call = &CodeGenerator::handle_event_filter_call;
    static constexpr CodegenHandler handle_event_map_call = &CodeGenerator::handle_event_map_call;
    static constexpr CodegenHandler handle_fast_call = &CodeGenerator::handle_fast_call;
    static constexpr CodegenHandler handle_freqs_call = &CodeGenerator::handle_freqs_call;
    static constexpr CodegenHandler handle_get_call = &CodeGenerator::handle_get_call;
    static constexpr CodegenHandler handle_harmonics_call = &CodeGenerator::handle_harmonics_call;
    static constexpr CodegenHandler handle_input_call = &CodeGenerator::handle_input_call;
    static constexpr CodegenHandler handle_iter_back_call = &CodeGenerator::handle_iter_back_call;
    static constexpr CodegenHandler handle_iter_call = &CodeGenerator::handle_iter_call;
    static constexpr CodegenHandler handle_key_deltas_call = &CodeGenerator::handle_key_deltas_call;
    static constexpr CodegenHandler handle_left_call = &CodeGenerator::handle_left_call;
    static constexpr CodegenHandler handle_len_call = &CodeGenerator::handle_len_call;
    static constexpr CodegenHandler handle_linger_call = &CodeGenerator::handle_linger_call;
    static constexpr CodegenHandler handle_linspace_call = &CodeGenerator::handle_linspace_call;
    static constexpr CodegenHandler handle_map_call = &CodeGenerator::handle_map_call;
    static constexpr CodegenHandler handle_mean_call = &CodeGenerator::handle_mean_call;
    static constexpr CodegenHandler handle_midi_cc_call = &CodeGenerator::handle_midi_cc_call;
    static constexpr CodegenHandler handle_minmax_call = &CodeGenerator::handle_minmax_call;
    static constexpr CodegenHandler handle_mixer_call = &CodeGenerator::handle_mixer_call;
    static constexpr CodegenHandler handle_mode_call = &CodeGenerator::handle_mode_call;
    static constexpr CodegenHandler handle_ms_decode_call = &CodeGenerator::handle_ms_decode_call;
    static constexpr CodegenHandler handle_ms_encode_call = &CodeGenerator::handle_ms_encode_call;
    static constexpr CodegenHandler handle_normalize_call = &CodeGenerator::handle_normalize_call;
    static constexpr CodegenHandler handle_notes_call = &CodeGenerator::handle_notes_call;
    static constexpr CodegenHandler handle_oscilloscope_call = &CodeGenerator::handle_oscilloscope_call;
    static constexpr CodegenHandler handle_palindrome_call = &CodeGenerator::handle_palindrome_call;
    static constexpr CodegenHandler handle_param_call = &CodeGenerator::handle_param_call;
    static constexpr CodegenHandler handle_pianoroll_call = &CodeGenerator::handle_pianoroll_call;
    static constexpr CodegenHandler handle_ply_call = &CodeGenerator::handle_ply_call;
    static constexpr CodegenHandler handle_random_call = &CodeGenerator::handle_random_call;
    static constexpr CodegenHandler handle_range_call = &CodeGenerator::handle_range_call;
    static constexpr CodegenHandler handle_reduce_call = &CodeGenerator::handle_reduce_call;
    static constexpr CodegenHandler handle_repeat_call = &CodeGenerator::handle_repeat_call;
    static constexpr CodegenHandler handle_rev_call = &CodeGenerator::handle_rev_call;
    static constexpr CodegenHandler handle_reverse_call = &CodeGenerator::handle_reverse_call;
    static constexpr CodegenHandler handle_right_call = &CodeGenerator::handle_right_call;
    static constexpr CodegenHandler handle_rotate_call = &CodeGenerator::handle_rotate_call;
    static constexpr CodegenHandler handle_run_call = &CodeGenerator::handle_run_call;
    static constexpr CodegenHandler handle_samples_call = &CodeGenerator::handle_samples_call;
    static constexpr CodegenHandler handle_scalar_call = &CodeGenerator::handle_scalar_call;
    static constexpr CodegenHandler handle_segment_call = &CodeGenerator::handle_segment_call;
    static constexpr CodegenHandler handle_select_call = &CodeGenerator::handle_select_call;
    static constexpr CodegenHandler handle_set_call = &CodeGenerator::handle_set_call;
    static constexpr CodegenHandler handle_sf_voice_call = &CodeGenerator::handle_sf_voice_call;
    static constexpr CodegenHandler handle_shuffle_call = &CodeGenerator::handle_shuffle_call;
    static constexpr CodegenHandler handle_slow_call = &CodeGenerator::handle_slow_call;
    static constexpr CodegenHandler handle_sort_call = &CodeGenerator::handle_sort_call;
    static constexpr CodegenHandler handle_soundfont_call = &CodeGenerator::handle_soundfont_call;
    static constexpr CodegenHandler handle_spectrum_call = &CodeGenerator::handle_spectrum_call;
    static constexpr CodegenHandler handle_spread_call = &CodeGenerator::handle_spread_call;
    static constexpr CodegenHandler handle_state_call = &CodeGenerator::handle_state_call;
    static constexpr CodegenHandler handle_stereo_call = &CodeGenerator::handle_stereo_call;
    static constexpr CodegenHandler handle_sum_call = &CodeGenerator::handle_sum_call;
    static constexpr CodegenHandler handle_take_call = &CodeGenerator::handle_take_call;
    static constexpr CodegenHandler handle_tap_delay_call = &CodeGenerator::handle_tap_delay_call;
    static constexpr CodegenHandler handle_timeline_call = &CodeGenerator::handle_timeline_call;
    static constexpr CodegenHandler handle_toggle_call = &CodeGenerator::handle_toggle_call;
    static constexpr CodegenHandler handle_tune_call = &CodeGenerator::handle_tune_call;
    static constexpr CodegenHandler handle_variant_call = &CodeGenerator::handle_variant_call;
    static constexpr CodegenHandler handle_voicing_call = &CodeGenerator::handle_voicing_call;
    static constexpr CodegenHandler handle_waterfall_call = &CodeGenerator::handle_waterfall_call;
    static constexpr CodegenHandler handle_waveform_call = &CodeGenerator::handle_waveform_call;
    static constexpr CodegenHandler handle_when_call = &CodeGenerator::handle_when_call;
    static constexpr CodegenHandler handle_width_call = &CodeGenerator::handle_width_call;
    static constexpr CodegenHandler handle_wt_load_call = &CodeGenerator::handle_wt_load_call;
    static constexpr CodegenHandler handle_zipWith_call = &CodeGenerator::handle_zipWith_call;
    static constexpr CodegenHandler handle_zip_call = &CodeGenerator::handle_zip_call;
    static constexpr CodegenHandler handle_zoom_call = &CodeGenerator::handle_zoom_call;
};

inline constexpr auto BUILTIN_FUNCTIONS = frozen::make_unordered_map<frozen::string, BuiltinInfo>({
    // Basic Oscillators
    // All oscillators now support optional phase offset and trigger for phase reset.
    // Phase/trig default to BUFFER_UNUSED, which falls back to BUFFER_ZERO (always 0.0).
    // This avoids emitting PUSH_CONST instructions for the common case.
    {"tri",     {cedar::Opcode::OSC_TRI,    1, 2, true,
                 {"freq", "phase", "trig", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Triangle wave oscillator"}},
    {"saw",     {cedar::Opcode::OSC_SAW,    1, 2, true,
                 {"freq", "phase", "trig", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Band-limited sawtooth oscillator"}},
    {"sqr",     {cedar::Opcode::OSC_SQR,    1, 2, true,
                 {"freq", "phase", "trig", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Band-limited square wave oscillator"}},
    {"ramp",    {cedar::Opcode::OSC_RAMP,   1, 2, true,
                 {"freq", "phase", "trig", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Rising ramp oscillator (0 to 1)"}},
    {"phasor",  {cedar::Opcode::OSC_PHASOR, 1, 2, true,
                 {"freq", "phase", "trig", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Phase accumulator (0 to 1 ramp)"}},
    {"sqr_minblep", {cedar::Opcode::OSC_SQR_MINBLEP, 1, 2, true,
                 {"freq", "phase", "trig", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "MinBLEP anti-aliased square wave"}},
    {"sine",    {cedar::Opcode::OSC_SIN,   1, 2, true,
                 {"freq", "phase", "trig", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Sine wave oscillator"}},

    // PWM Oscillators (2 inputs: frequency, pwm amount + optional phase/trig)
    {"sqr_pwm", {cedar::Opcode::OSC_SQR_PWM, 2, 2, true,
                 {"freq", "pwm", "phase", "trig", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Pulse width modulated square wave"}},
    {"saw_pwm", {cedar::Opcode::OSC_SAW_PWM, 2, 2, true,
                 {"freq", "pwm", "phase", "trig", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Variable-width sawtooth oscillator"}},
    {"sqr_pwm_minblep", {cedar::Opcode::OSC_SQR_PWM_MINBLEP, 2, 2, true,
                 {"freq", "pwm", "phase", "trig", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "MinBLEP PWM square wave"}},

    // 4x Oversampled PWM (explicit, for when auto-detection isn't desired)
    {"sqr_pwm_4x", {cedar::Opcode::OSC_SQR_PWM_4X, 2, 2, true,
                 {"freq", "pwm", "phase", "trig", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "4x oversampled PWM square wave"}},
    {"saw_pwm_4x", {cedar::Opcode::OSC_SAW_PWM_4X, 2, 2, true,
                 {"freq", "pwm", "phase", "trig", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "4x oversampled PWM sawtooth"}},

    // Wavetable oscillator (Smooch).
    //   smooch("bank_name", freq)
    //   smooch("bank_name", freq, phase)
    //   smooch("bank_name", freq, phase, tablePos)
    // The bank must have been declared earlier via wt_load("bank_name", "path").
    // All three names (smooch / wt / wavetable) are special-handled in
    // codegen — the entries here exist so the analyzer recognizes the
    // identifier and reports unknown-function errors correctly. The PRD
    // also listed "wave" as an alias but that collides with a common
    // variable name (existing tests use `wave = ...`), so we ship three.
    {"smooch",    {cedar::Opcode::OSC_WAVETABLE, 2, 2, true,
                   {"bank", "freq", "phase", "tablePos", "", ""},
                   {0.0f, 0.0f, NAN, NAN, NAN},
                   "Wavetable oscillator (Smooch). smooch(\"bank\", freq, phase?, tablePos?)."}},
    {"wt",        {cedar::Opcode::OSC_WAVETABLE, 2, 2, true,
                   {"bank", "freq", "phase", "tablePos", "", ""},
                   {0.0f, 0.0f, NAN, NAN, NAN},
                   "Wavetable oscillator (alias for smooch)."}},
    {"wavetable", {cedar::Opcode::OSC_WAVETABLE, 2, 2, true,
                   {"bank", "freq", "phase", "tablePos", "", ""},
                   {0.0f, 0.0f, NAN, NAN, NAN},
                   "Wavetable oscillator (alias for smooch)."}},

    // wt_load — compile-time directive that registers a wavetable bank for
    // the host to load. Mirrors `soundfont` in shape: opcode = NOP because
    // there is no audio-time instruction; codegen special-handles it (see
    // codegen_handler dispatch) to extract the string-literal args
    // into result.required_wavetables. The host loads the bank after compile.
    {"wt_load",   {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"name", "path", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN, NAN, NAN},
                 .description = "Load a wavetable bank (compile-time): wt_load(\"name\", \"path\").",
                 .codegen_handler = BuiltinHandlers::handle_wt_load_call}},

    // samples — compile-time directive that declares a sample-bank URI for
    // the host to load. Mirrors wt_load in shape: opcode = NOP, special-cased
    // by codegen (via codegen_handler dispatch) to extract the string-
    // literal argument and append a UriRequest to `required_uris_`. The host
    // fetches each URI via the resolver before swapping bytecode.
    {"samples",   {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"uri", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN, NAN, NAN},
                 .description = "Declare a sample-bank URI (compile-time): samples(\"github:user/repo\").",
                 .codegen_handler = BuiltinHandlers::handle_samples_call}},

    // Filters (signal, cutoff required; q optional with default 0.707)
    // Stereo-native (prd-stereo-native-opcodes Phase 4a): per-channel filter
    // memory inside one state struct; coefficient cache shared across L/R
    // because freq/q are mono control signals. Mono input auto-escalates.
    // SVF (State Variable Filter) - stable under modulation
    // Category B: dry/wet via slots 3-4 (default dry=0.0, wet=1.0 = back-compat)
    {"lp",      {cedar::Opcode::FILTER_SVF_LP, 2, 3, true,
                 {"in", "cut", "q", "dry", "wet", ""},
                 {0.707f, 0.0f, 1.0f, NAN, NAN},
                 "State-variable lowpass filter",
                 0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    {"hp",      {cedar::Opcode::FILTER_SVF_HP, 2, 3, true,
                 {"in", "cut", "q", "dry", "wet", ""},
                 {0.707f, 0.0f, 1.0f, NAN, NAN},
                 "State-variable highpass filter",
                 0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    {"bp",      {cedar::Opcode::FILTER_SVF_BP, 2, 3, true,
                 {"in", "cut", "q", "dry", "wet", ""},
                 {0.707f, 0.0f, 1.0f, NAN, NAN},
                 "State-variable bandpass filter",
                 0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    // Moog ladder filter (4-pole with resonance)
    // Optional: max_resonance (self-oscillation threshold), input_scale (preamp drive)
    // Category B: dry/wet via ExtendedParams (input slots full).
    {"moog",    {.opcode = cedar::Opcode::FILTER_MOOG,
                 .input_count = 2, .optional_count = 3, .requires_state = true,
                 .param_names = {"in", "cut", "res", "max_res", "input_scale", ""},
                 .defaults = {1.0f, 4.0f, 0.5f, NAN, NAN},
                 .description = "Moog 4-pole ladder filter with resonance",
                 .extended_param_count = 2,
                 .output_channels = ChannelCount::Stereo,
                 .stereo_native = true,
                 .extended_param_names = {"dry", "wet"},
                 .extended_defaults = {0.0f, 1.0f}}},
    // Diode ladder filter (TB-303 acid) - 5 inputs: in, cut, res, vt, fb_gain
    // Category B: dry/wet via ExtendedParams.
    {"diode",   {.opcode = cedar::Opcode::FILTER_DIODE,
                 .input_count = 2, .optional_count = 3, .requires_state = true,
                 .param_names = {"in", "cut", "res", "vt", "fb_gain", ""},
                 .defaults = {1.0f, 0.026f, 10.0f},
                 .description = "TB-303 style diode ladder filter",
                 .extended_param_count = 2,
                 .output_channels = ChannelCount::Stereo,
                 .stereo_native = true,
                 .extended_param_names = {"dry", "wet"},
                 .extended_defaults = {0.0f, 1.0f}}},
    // Formant filter (vowel morphing) - 5 inputs: in, vowel_a, vowel_b, morph, q
    // Category B: dry/wet via ExtendedParams.
    {"formant", {.opcode = cedar::Opcode::FILTER_FORMANT,
                 .input_count = 2, .optional_count = 3, .requires_state = true,
                 .param_names = {"in", "vowel_a", "vowel_b", "morph", "q", ""},
                 .defaults = {0.0f, 0.5f, 10.0f},
                 .description = "Vowel formant filter with morphing",
                 .extended_param_count = 2,
                 .output_channels = ChannelCount::Stereo,
                 .stereo_native = true,
                 .extended_param_names = {"dry", "wet"},
                 .extended_defaults = {0.0f, 1.0f}}},
    // Sallen-Key filter (MS-20 style) - 5 inputs: in, cut, res, mode, clip_threshold
    // Optional: clip_threshold (feedback clipping point)
    // Category B: dry/wet via ExtendedParams.
    {"sallenkey", {.opcode = cedar::Opcode::FILTER_SALLENKEY,
                   .input_count = 2, .optional_count = 3, .requires_state = true,
                   .param_names = {"in", "cut", "res", "mode", "clip_thresh", ""},
                   .defaults = {1.0f, 0.0f, 0.7f, NAN, NAN},
                   .description = "MS-20 style Sallen-Key filter",
                   .extended_param_count = 2,
                   .output_channels = ChannelCount::Stereo,
                   .stereo_native = true,
                   .extended_param_names = {"dry", "wet"},
                   .extended_defaults = {0.0f, 1.0f}}},

    // Envelopes
    {"adsr",    {cedar::Opcode::ENV_ADSR, 1, 4, true,
                 {"gate", "attack", "decay", "sustain", "release", ""},
                 {0.01f, 0.1f, 0.5f},
                 "Attack-decay-sustain-release envelope"}},
    {"ar",      {cedar::Opcode::ENV_AR, 1, 2, true,
                 {"trig", "attack", "release", "", "", ""},
                 {0.01f, 0.3f, NAN},
                 "Attack-release envelope (one-shot)"}},
    // env_follower — stereo-native (prd-stereo-native-opcodes Phase 4d).
    // Per-channel envelope levels in a dedicated EnvFollowerState.
    // Output width matches input: mono in → mono out (so the CV slots into
    // downstream mono parameter slots without E186); stereo in → per-channel
    // envelopes (per-channel sidechain). adsr/ar stay mono per PRD §3.2.
    {"env_follower", {cedar::Opcode::ENV_FOLLOWER, 1, 2, true,
                      {"in", "attack", "release", "", "", ""},
                      {0.01f, 0.1f, NAN},
                      "Amplitude envelope follower",
                      0, {}, {}, ChannelCount::Match, /*stereo_native=*/true}},

    // Samplers (stereo-native, prd-stereo-native-opcodes Phase 3): mono files
    // broadcast L=R; stereo files preserve channels; 3+ channel files keep the
    // first two and drop the rest.
    {"sample",  {cedar::Opcode::SAMPLE_PLAY, 3, 0, true,
                 {"trig", "pitch", "id", "", "", ""},
                 {NAN, NAN, NAN},
                 "Stereo-native one-shot sample playback. id accepts a sample "
                 "name (\"bd\", \"bd:3\", \"Bank/name:variant\") or numeric "
                 "sample-bank ID.",
                 0, {}, {}, ChannelCount::Stereo,
                 /*stereo_native=*/true}},
    {"sample_loop", {cedar::Opcode::SAMPLE_PLAY_LOOP, 3, 0, true,
                     {"gate", "pitch", "id", "", "", ""},
                     {NAN, NAN, NAN},
                     "Stereo-native looping sample playback. id accepts a "
                     "sample name or numeric sample-bank ID.",
                     0, {}, {}, ChannelCount::Stereo,
                     /*stereo_native=*/true}},
    // PRD prd-midi-input §7.1: requires_state flipped to true so the state
    // pool retains seq_state_id + preset_idx across blocks for the MIDI
    // event-driven path. Legacy pattern path is unaffected (its codegen
    // assigns one state_id per chord voice and lets the runtime alloc).
    {"soundfont", {.opcode = cedar::Opcode::NOP, .input_count = 2, .optional_count = 1, .requires_state = true,
                   .param_names = {"input", "file", "preset", "", "", ""},
                   .defaults = {NAN, NAN, NAN, NAN, NAN},
                   .description = "SoundFont playback: soundfont(pattern, \"file.sf2\", preset). Accepts midi() upstream for live polyphonic SF2 playback.",
                   .consumes_polyphonic_pattern = true,
                 .codegen_handler = BuiltinHandlers::handle_soundfont_call}},
    // Single-voice SoundFont player. Custom codegen (handle_sf_voice_call)
    // resolves `file` (literal path or $soundfont_alias) and `preset` at
    // compile time, then emits one SF_VOICE driven by the freq/gate/vel
    // signals — so it slots into poly() like any other instrument.
    {"sf_voice", {.opcode = cedar::Opcode::SF_VOICE, .input_count = 5, .optional_count = 0, .requires_state = true,
                  .param_names = {"file", "preset", "freq", "gate", "vel", ""},
                  .defaults = {NAN, NAN, NAN, NAN, NAN},
                  .description = "Single-voice SoundFont player: sf_voice(file, preset, freq, gate, vel). Stereo output; designed as an instrument for poly().",
                  .output_channels = ChannelCount::Stereo, .stereo_native = true,
                 .codegen_handler = BuiltinHandlers::handle_sf_voice_call}},
    // PRD prd-midi-input §4.7: runtime MIDI event source. The `options` record
    // selects the source (default device / named device / .mid file) and tunes
    // the live filter. Special-cased in codegen as handle_midi_call.
    {"midi", {.opcode = cedar::Opcode::MIDI_QUERY,
              .input_count = 0, .optional_count = 1, .requires_state = true,
              .param_names = {"options", "", "", "", "", ""},
              .defaults = {NAN, NAN, NAN, NAN, NAN},
              .description = "MIDI event source. Bare midi() opens the default live device. "
                             "midi({device: name}) opens a named device; midi({file: path}) "
                             "plays a .mid file. Options: device, file, channel, loop, tempo.",
              .param_types = {ParamValueType::Record},
              .option_schemas = {OptionSchema{
                  /*param_index=*/0,
                  /*fields=*/{{
                      {"device",  OptionFieldType::String, "",          "Live device name (substring match against host port list)"},
                      {"file",    OptionFieldType::String, "",          "Path/URI of a .mid file (mutually exclusive with device:)"},
                      {"channel", OptionFieldType::Number, "0",         "Channel filter, 1-16 (0 = any)"},
                      {"loop",    OptionFieldType::Bool,   "false",     "Loop file playback at end-of-track"},
                      {"tempo",   OptionFieldType::Enum,   "\"follow\"", "Playback tempo policy (file mode only)", "follow,file"},
                  }},
                  /*field_count=*/5,
              }},
              .option_schema_count = 1}},

    // PRD prd-midi-input §4.8: route an incoming MIDI CC / pitch-bend / channel-
    // aftertouch to a named param() slot via EnvMap. Compile-time directive only
    // (no bytecode emitted); the host MIDI callback evaluates the route and
    // calls vm.set_param(name, value, slew_ms). Special-cased in codegen as
    // handle_midi_cc_call. Defaults: cc 0..1 range, pb -1..+1 range; 5 ms slew.
    {"midi_cc", {.opcode = cedar::Opcode::NOP,
                 .input_count = 1, .optional_count = 1, .requires_state = false,
                 .param_names = {"name", "options", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN, NAN, NAN},
                 .description = "Route an incoming MIDI CC / pitch-bend / channel-aftertouch to a "
                                "named param() slot via EnvMap. Compile-time only — exactly one of "
                                "cc:, pb:, at: must be set. Defaults: 0..1 range (cc/at), -1..+1 (pb).",
                 .param_types = {ParamValueType::String, ParamValueType::Record},
                 .option_schemas = {OptionSchema{
                     /*param_index=*/1,
                     /*fields=*/{{
                         {"cc",      OptionFieldType::Number, "-128",  "CC number 0..127 (omit when pb: or at: is set)"},
                         {"channel", OptionFieldType::Number, "0",     "Channel filter, 1-16 (0 = any)"},
                         {"pb",      OptionFieldType::Bool,   "false", "Route 14-bit pitch-bend instead of a CC"},
                         {"at",      OptionFieldType::Bool,   "false", "Route channel aftertouch instead of a CC"},
                         {"min",     OptionFieldType::Number, "0",     "Output range minimum (defaults to -1 when pb: is set)"},
                         {"max",     OptionFieldType::Number, "1",     "Output range maximum"},
                         {"slew",    OptionFieldType::Number, "5",     "EnvMap slew, in milliseconds"},
                     }},
                     /*field_count=*/7,
                 }},
                 .option_schema_count = 1,
                 .codegen_handler = BuiltinHandlers::handle_midi_cc_call}},

    // Delays — stereo-native (prd-stereo-native-opcodes Phase 4c).
    // Per-channel ring buffers; mono control inputs (time/fb/dry/wet) shared.
    // Category A (parallel-mix): defaults dry=1.0, wet=0.5.
    {"delay",   {cedar::Opcode::DELAY, 3, 2, true,
                 {"in", "time", "fb", "dry", "wet", ""},
                 {1.0f, 0.5f, NAN, NAN, NAN},
                 "Delay line (time in seconds, 0-10)",
                 0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    // Delay variants with different time units
    {"delay_ms",    {cedar::Opcode::DELAY, 3, 2, true,
                     {"in", "time_ms", "fb", "dry", "wet", ""},
                     {1.0f, 0.5f, NAN, NAN, NAN},
                     "Delay line (time in milliseconds, 0-10000)",
                     0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true, /*inst_rate=*/1}},
    {"delay_smp",   {cedar::Opcode::DELAY, 3, 2, true,
                     {"in", "time_smp", "fb", "dry", "wet", ""},
                     {1.0f, 0.5f, NAN, NAN, NAN},
                     "Delay line (time in samples, direct control)",
                     0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true, /*inst_rate=*/2}},
    // Tap delay with configurable feedback processing (handled specially by codegen)
    // tap_delay(in, time, fb, processor) where processor is a closure: (x) -> ...
    // The closure receives the delayed signal and its output is mixed back with feedback.
    // Category A (parallel-mix): defaults dry=1.0, wet=0.5.
    {"tap_delay", {.opcode = cedar::Opcode::DELAY_TAP,
                 .input_count = 4,
                 .optional_count = 2,
                 .requires_state = true,
                 .param_names = {"in", "time", "fb", "processor", "dry", "wet"},
                 .defaults = {1.0f, 0.5f, NAN, NAN, NAN},
                 .description = "Tap delay with feedback chain (time in seconds)",
                 .codegen_handler = BuiltinHandlers::handle_tap_delay_call}},
    {"tap_delay_ms", {.opcode = cedar::Opcode::DELAY_TAP,
                 .input_count = 4,
                 .optional_count = 2,
                 .requires_state = true,
                 .param_names = {"in", "time_ms", "fb", "processor", "dry", "wet"},
                 .defaults = {1.0f, 0.5f, NAN, NAN, NAN},
                 .description = "Tap delay with feedback chain (time in milliseconds)",
                 .codegen_handler = BuiltinHandlers::handle_tap_delay_call}},
    {"tap_delay_smp", {.opcode = cedar::Opcode::DELAY_TAP,
                 .input_count = 4,
                 .optional_count = 2,
                 .requires_state = true,
                 .param_names = {"in", "time_smp", "fb", "processor", "dry", "wet"},
                 .defaults = {1.0f, 0.5f, NAN, NAN, NAN},
                 .description = "Tap delay with feedback chain (time in samples)",
                 .codegen_handler = BuiltinHandlers::handle_tap_delay_call}},

    // Reverbs (stateful - large delay networks)
    // All three reverbs are stereo-native (prd-stereo-native-opcodes Phases 0–2):
    // they produce a stereo output pair natively, auto-escalate mono input,
    // and read stereo primary input from inputs[0] (L) / inputs[0]+1 (R).
    // freeverb: room_scale (density factor), room_offset (decay baseline)
    // dry/wet via ExtendedParams (Category A: dry=1.0, wet=0.5)
    {"freeverb", {.opcode = cedar::Opcode::REVERB_FREEVERB,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "room", "damp", "room_scale", "room_offset", ""},
                  .defaults = {0.5f, 0.5f, 0.28f, 0.7f, NAN},
                  .description = "Freeverb algorithmic reverb",
                  .extended_param_count = 2,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"dry", "wet"},
                  .extended_defaults = {1.0f, 0.5f}}},
    // dattorro: input_diffusion (input smoothing), decay_diffusion (tail smoothing).
    // damping/mod_depth were bit-packed in inst.rate and lfo_rate was hardcoded
    // before prd-extended-params-migration; now ExtendedParams<5> alongside
    // dry/wet (Category A: dry=1.0, wet=0.5).
    {"dattorro", {.opcode = cedar::Opcode::REVERB_DATTORRO,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "decay", "predelay", "in_diff", "dec_diff", ""},
                  .defaults = {0.7f, 20.0f, 0.75f, 0.625f, NAN},
                  .description = "Dattorro plate reverb algorithm",
                  .extended_param_count = 5,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"damping", "mod_depth", "lfo_rate", "dry", "wet"},
                  .extended_defaults = {0.0f, 0.0f, 0.5f, 1.0f, 0.5f}}},
    // fdn has free input slots; dry/wet via slots 3-4 (Category A: dry=1.0, wet=0.5)
    {"fdn",      {cedar::Opcode::REVERB_FDN, 1, 4, true,
                  {"in", "decay", "damp", "dry", "wet", ""},
                  {0.8f, 0.3f, 1.0f, 0.5f, NAN},
                  "Feedback delay network reverb",
                  0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},

    // Modulation Effects (stateful - delay lines with LFOs)
    // All three are stereo-native (prd-stereo-native-opcodes Phase 3): mono
    // input auto-escalates to L=R; stereo input drives per-channel processing.
    // R lane reads the shared master LFO at the user-tunable `lfo_phase`
    // offset (turns, default 0.25 = 90°) for L/R decorrelation.
    // chorus: base_delay (ms), depth_range (ms), lfo_phase (turns)
    // dry/wet appended to ExtendedParams (Category A: dry=1.0, wet=0.5).
    {"chorus",   {.opcode = cedar::Opcode::EFFECT_CHORUS,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "rate", "depth", "base_delay", "depth_range", ""},
                  .defaults = {0.5f, 0.5f, 20.0f, 10.0f, NAN},
                  .description = "Stereo-native chorus (mono input widens; stereo input "
                                 "processes per channel). lfo_phase tunes the R-LFO offset.",
                  .extended_param_count = 3,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"lfo_phase", "dry", "wet"},
                  .extended_defaults = {0.25f, 1.0f, 0.5f}}},
    // flanger: min_delay (ms), max_delay (ms). feedback was bit-packed in
    // inst.rate before prd-extended-params-migration; now ExtendedParams<5>
    // alongside lfo_phase + dry/wet (Category A: dry=1.0, wet=0.5).
    {"flanger",  {.opcode = cedar::Opcode::EFFECT_FLANGER,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "rate", "depth", "min_delay", "max_delay", ""},
                  .defaults = {1.0f, 0.7f, 0.1f, 10.0f, NAN},
                  .description = "Stereo-native flanger. lfo_phase tunes the R-LFO offset.",
                  .extended_param_count = 4,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"feedback", "lfo_phase", "dry", "wet"},
                  .extended_defaults = {-0.99f, 0.25f, 1.0f, 0.5f}}},
    // phaser: min_freq, max_freq, plus 5 extended params (feedback, stages,
    // lfo_phase, dry, wet). Feedback/stages used to be packed into inst.rate;
    // they are now full first-class extended params (PRD prd-extended-params §6b).
    // dry/wet appended (Category A: dry=1.0, wet=0.5).
    {"phaser",   {.opcode = cedar::Opcode::EFFECT_PHASER,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "rate", "depth", "min_freq", "max_freq", ""},
                  .defaults = {0.5f, 0.8f, 200.0f, 4000.0f, NAN},
                  .description = "Stereo-native multi-stage phaser. "
                                 "feedback (0-0.99), stages (2-12), lfo_phase (turns).",
                  .extended_param_count = 5,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"feedback", "stages", "lfo_phase", "dry", "wet"},
                  .extended_defaults = {0.5f, 4.0f, 0.25f, 1.0f, 0.5f}}},
    // comb: dry/wet via input slots 3-4 (Category A: dry=1.0, wet=0.5).
    // damping was packed in inst.rate before prd-extended-params-migration;
    // now ExtendedParams<1>.
    {"comb",     {.opcode = cedar::Opcode::EFFECT_COMB,
                  .input_count = 3, .optional_count = 2, .requires_state = true,
                  .param_names = {"in", "time", "fb", "dry", "wet", ""},
                  .defaults = {1.0f, 0.5f, NAN, NAN, NAN},
                  .description = "Comb filter (resonant delay). damping rolls off the tail.",
                  .extended_param_count = 1,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"damping"},
                  .extended_defaults = {0.0f}}},

    // Distortion — all stereo-native (prd-stereo-native-opcodes Phase 4b).
    // Per-channel state arrays inside one state struct; runtime params (drive,
    // threshold, frequency, etc.) are mono control signals shared across L/R.
    // Note: tanh(x) is now a pure math function. Use saturate(in, drive) for distortion.
    // Category B: dry/wet via slots 2-3 (defaults dry=0.0, wet=1.0 = back-compat).
    {"saturate", {cedar::Opcode::DISTORT_TANH, 1, 3, false,
                  {"in", "drive", "dry", "wet", "", ""},
                  {2.0f, 0.0f, 1.0f, NAN, NAN},
                  "Soft saturation (tanh) distortion",
                  0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    {"softclip", {cedar::Opcode::DISTORT_SOFT, 1, 3, false,
                  {"in", "thresh", "dry", "wet", "", ""},
                  {0.5f, 0.0f, 1.0f, NAN, NAN},
                  "Soft clipper distortion",
                  0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    // dry/wet via slots 3-4.
    {"bitcrush", {cedar::Opcode::DISTORT_BITCRUSH, 1, 4, true,
                  {"in", "bits", "rate", "dry", "wet", ""},
                  {8.0f, 0.5f, 0.0f, 1.0f, NAN},
                  "Bit depth and sample rate reducer",
                  0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    // fold: routed via ExtendedParams (opcode reads inputs[2]=symmetry; keep
    // BuiltinInfo legacy single-optional and add dry/wet as Extended slots).
    {"fold",     {.opcode = cedar::Opcode::DISTORT_FOLD,
                  .input_count = 1, .optional_count = 1, .requires_state = false,
                  .param_names = {"in", "thresh", "", "", "", ""},
                  .defaults = {0.5f, NAN, NAN},
                  .description = "Wavefolding distortion",
                  .extended_param_count = 2,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"dry", "wet"},
                  .extended_defaults = {0.0f, 1.0f}}},
    {"tube",     {cedar::Opcode::DISTORT_TUBE, 1, 4, true,
                  {"in", "drive", "bias", "dry", "wet", ""},
                  {5.0f, 0.1f, 0.0f, 1.0f, NAN},
                  "Tube amp emulation with bias",
                  0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    {"smooth",   {cedar::Opcode::DISTORT_SMOOTH, 1, 3, true,
                  {"in", "drive", "dry", "wet", "", ""},
                  {5.0f, 0.0f, 1.0f, NAN, NAN},
                  "ADAA alias-free saturation",
                  0, {}, {}, ChannelCount::Stereo, /*stereo_native=*/true}},
    // tape: soft_threshold (saturation onset), warmth_scale (HF rolloff amount)
    // dry/wet via ExtendedParams (input slots full).
    {"tape",     {.opcode = cedar::Opcode::DISTORT_TAPE,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "drive", "warmth", "soft_thresh", "warmth_scale", ""},
                  .defaults = {3.0f, 0.3f, 0.5f, 0.7f, NAN},
                  .description = "Tape saturation with warmth",
                  .extended_param_count = 2,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"dry", "wet"},
                  .extended_defaults = {0.0f, 1.0f}}},
    // xfmr: bass_freq (bass extraction cutoff Hz)
    // dry/wet via ExtendedParams.
    {"xfmr",     {.opcode = cedar::Opcode::DISTORT_XFMR,
                  .input_count = 1, .optional_count = 3, .requires_state = true,
                  .param_names = {"in", "drive", "bass", "bass_freq", "", ""},
                  .defaults = {3.0f, 5.0f, 60.0f, NAN, NAN},
                  .description = "Transformer saturation with bass boost",
                  .extended_param_count = 2,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"dry", "wet"},
                  .extended_defaults = {0.0f, 1.0f}}},
    // excite: harmonic_odd (odd harmonic mix), harmonic_even (even harmonic mix)
    // dry/wet via ExtendedParams.
    {"excite",   {.opcode = cedar::Opcode::DISTORT_EXCITE,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "amount", "freq", "harm_odd", "harm_even", ""},
                  .defaults = {0.5f, 3000.0f, 0.4f, 0.6f, NAN},
                  .description = "Aural exciter (harmonic enhancer)",
                  .extended_param_count = 2,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"dry", "wet"},
                  .extended_defaults = {0.0f, 1.0f}}},

    // Dynamics — stereo-native (prd-stereo-native-opcodes Phase 4d).
    // Per-channel envelope/gain state; coefficient cache shared.
    // Dynamics: Category B (transform). dry/wet defaults dry=0.0, wet=1.0
    // (back-compat). Set dry>0 for parallel/NY compression.
    // comp: attack/release (ms) via ExtendedParams<2> — were bit-packed in
    // inst.rate before prd-extended-params-migration. dry/wet stay in slots 3-4.
    {"comp",     {.opcode = cedar::Opcode::DYNAMICS_COMP,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "thresh", "ratio", "dry", "wet", ""},
                  .defaults = {-12.0f, 4.0f, 0.0f, 1.0f, NAN},
                  .description = "Dynamic range compressor. attack/release in ms.",
                  .extended_param_count = 2,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"attack", "release"},
                  .extended_defaults = {0.1f, 10.0f}}},
    // limiter: lookahead (ms, 0 = off) via ExtendedParams<1> — replaced the
    // inst.rate boolean toggle. dry/wet stay in slots 3-4.
    {"limiter",  {.opcode = cedar::Opcode::DYNAMICS_LIMITER,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "ceiling", "release", "dry", "wet", ""},
                  .defaults = {-0.1f, 0.1f, 0.0f, 1.0f, NAN},
                  .description = "Peak limiter. lookahead in ms (0 = off).",
                  .extended_param_count = 1,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"lookahead"},
                  .extended_defaults = {0.0f}}},
    // gate: hysteresis (dB open/close diff), close_time (ms fade-out).
    // attack/hold/release (ms) were bit-packed in inst.rate before
    // prd-extended-params-migration; now ExtendedParams<5> alongside dry/wet
    // (input slots full).
    {"gate",     {.opcode = cedar::Opcode::DYNAMICS_GATE,
                  .input_count = 1, .optional_count = 4, .requires_state = true,
                  .param_names = {"in", "thresh", "range", "hyst", "close_time", ""},
                  .defaults = {-40.0f, -40.0f, 6.0f, 5.0f, NAN},
                  .description = "Noise gate with hysteresis. attack/hold/release in ms.",
                  .extended_param_count = 5,
                  .output_channels = ChannelCount::Stereo,
                  .stereo_native = true,
                  .extended_param_names = {"attack", "hold", "release", "dry", "wet"},
                  .extended_defaults = {0.1f, 0.0f, 10.0f, 0.0f, 1.0f}}},

    // Arithmetic (2 inputs, stateless) - from binary operator desugaring
    {"add",     {cedar::Opcode::ADD, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Add two signals"}},
    {"sub",     {cedar::Opcode::SUB, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Subtract two signals"}},
    {"mul",     {cedar::Opcode::MUL, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Multiply two signals"}},
    {"div",     {cedar::Opcode::DIV, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Divide two signals"}},
    {"pow",     {cedar::Opcode::POW, 2, 0, false,
                 {"base", "exp", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Raise base to exponent power"}},

    // Math unary (1 input)
    {"neg",     {cedar::Opcode::NEG,   1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Negate signal (flip sign)"}},
    {"abs",     {cedar::Opcode::ABS,   1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Absolute value"}},
    {"sqrt",    {cedar::Opcode::SQRT,  1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Square root"}},
    {"log",     {cedar::Opcode::LOG,   1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Natural logarithm"}},
    {"exp",     {cedar::Opcode::EXP,   1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Exponential (e^x)"}},
    {"floor",   {cedar::Opcode::FLOOR, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Round down to integer"}},
    {"ceil",    {cedar::Opcode::CEIL,  1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Round up to integer"}},
    {"fmod",    {cedar::Opcode::FMOD,  2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "C-style float remainder: fmod(a,b); sign follows a"}},

    // Math - Trigonometric (radians)
    // NOTE: sin(x) is the mathematical sine function, NOT a sine oscillator!
    // Use sine(freq) for a sine wave oscillator.
    {"sin",     {cedar::Opcode::MATH_SIN,  1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Sine function (radians)"}},
    {"cos",     {cedar::Opcode::MATH_COS,  1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Cosine function (radians)"}},
    {"tan",     {cedar::Opcode::MATH_TAN,  1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Tangent function (radians)"}},
    {"asin",    {cedar::Opcode::MATH_ASIN, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Inverse sine (arc sine)"}},
    {"acos",    {cedar::Opcode::MATH_ACOS, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Inverse cosine (arc cosine)"}},
    {"atan",    {cedar::Opcode::MATH_ATAN, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Inverse tangent (arc tangent)"}},
    {"atan2",   {cedar::Opcode::MATH_ATAN2, 2, 0, false,
                 {"y", "x", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Two-argument arc tangent"}},

    // Math - Hyperbolic
    {"sinh",    {cedar::Opcode::MATH_SINH, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Hyperbolic sine"}},
    {"cosh",    {cedar::Opcode::MATH_COSH, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Hyperbolic cosine"}},
    // Pure mathematical tanh - useful for waveshaping: tanh(signal * drive)
    // For convenience distortion with drive parameter, use the tanh effect
    {"tanh",    {cedar::Opcode::MATH_TANH, 1, 0, false,
                 {"x", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Hyperbolic tangent (soft clipper)"}},

    // Math binary (2 inputs)
    // min/max can be binary or unary (reduction over array)
    {"min",     {.opcode = cedar::Opcode::MIN,
                 .input_count = 1,
                 .optional_count = 1,
                 .requires_state = false,
                 .param_names = {"a", "b", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Minimum: min(a, b) or min(array)",
                 .codegen_handler = BuiltinHandlers::handle_minmax_call}},
    {"max",     {.opcode = cedar::Opcode::MAX,
                 .input_count = 1,
                 .optional_count = 1,
                 .requires_state = false,
                 .param_names = {"a", "b", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Maximum: max(a, b) or max(array)",
                 .codegen_handler = BuiltinHandlers::handle_minmax_call}},

    // Math ternary (3 inputs)
    {"clamp",   {cedar::Opcode::CLAMP, 3, 0, false,
                 {"x", "lo", "hi", "", "", ""},
                 {NAN, NAN, NAN},
                 "Clamp value between lo and hi"}},
    {"wrap",    {cedar::Opcode::WRAP,  3, 0, false,
                 {"x", "lo", "hi", "", "", ""},
                 {NAN, NAN, NAN},
                 "Wrap value between lo and hi"}},

    // Conditionals - Signal Selection
    {"select",  {cedar::Opcode::SELECT, 3, 0, false,
                 {"cond", "a", "b", "", "", ""},
                 {NAN, NAN, NAN},
                 "Select between signals: (cond > 0) ? a : b"}},

    // Conditionals - Block-rate bypass. opcode = NOP: handle_when_call in
    // codegen_control_flow.cpp lowers this to SKIP_IF_* opcodes before the
    // generic emission path runs. Entry exists for arity-checking + autocomplete.
    {"when",    {.opcode = cedar::Opcode::NOP,
                 .input_count = 3,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"cond", "true_branch", "false_branch", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Block-rate conditional bypass: runs only the taken branch",
                 .codegen_handler = BuiltinHandlers::handle_when_call}},

    // Conditionals - Comparisons (return 0.0 or 1.0)
    {"gt",      {cedar::Opcode::CMP_GT, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Greater than: (a > b) ? 1 : 0"}},
    {"lt",      {cedar::Opcode::CMP_LT, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Less than: (a < b) ? 1 : 0"}},
    {"gte",     {cedar::Opcode::CMP_GTE, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Greater or equal: (a >= b) ? 1 : 0"}},
    {"lte",     {cedar::Opcode::CMP_LTE, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Less or equal: (a <= b) ? 1 : 0"}},
    {"eq",      {cedar::Opcode::CMP_EQ, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Approximate equality: |a - b| < epsilon ? 1 : 0"}},
    {"neq",     {cedar::Opcode::CMP_NEQ, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Not equal: |a - b| >= epsilon ? 1 : 0"}},

    // Conditionals - Logical Operations
    {"band",    {cedar::Opcode::LOGIC_AND, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Logical AND: (a > 0 && b > 0) ? 1 : 0"}},
    {"bor",     {cedar::Opcode::LOGIC_OR, 2, 0, false,
                 {"a", "b", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Logical OR: (a > 0 || b > 0) ? 1 : 0"}},
    {"bnot",    {cedar::Opcode::LOGIC_NOT, 1, 0, false,
                 {"a", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Logical NOT: (a > 0) ? 0 : 1"}},

    // Live audio input — produces a stereo signal sourced from the host
    // (microphone, tab/system audio, uploaded file, etc.). The optional source
    // string is a compile-time literal: "mic" | "tab" | "file:NAME". Codegen
    // emits an INPUT instruction and forwards the source string to the host
    // via the required-input-source table. Output is always Stereo.
    {"in",      {.opcode = cedar::Opcode::INPUT,
                 .input_count = 0,
                 .optional_count = 1,
                 .requires_state = false,
                 .param_names = {"source", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN, NAN, NAN},
                 .description = "Live audio input. Optional source: 'mic' (default), 'tab', 'file:NAME'.",
                 .extended_param_count = 0,
                 .param_types = {ParamValueType::String, {}, {}, {}, {}, {}},
                 .input_channels = {},
                 .output_channels = ChannelCount::Stereo,
                 .codegen_handler = BuiltinHandlers::handle_input_call}},

    // Utility
    {"noise",   {cedar::Opcode::NOISE, 0, 3, true,
                 {"freq", "trig", "seed", "", "", ""},
                 {NAN, NAN, NAN, NAN, NAN},
                 "Noise generator (freq=0: white, freq>0: sample-and-hold)"}},
    {"mtof",    {cedar::Opcode::MTOF,  1, 0, false,
                 {"note", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "MIDI note number to frequency (Hz)"}},
    {"dc",      {cedar::Opcode::DC,    1, 0, false,
                 {"offset", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "DC offset (constant value)"}},
    {"slew",    {cedar::Opcode::SLEW,  2, 0, true,
                 {"target", "rate", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Slew rate limiter (portamento) — output width matches input",
                 0, {}, {}, ChannelCount::Match, /*stereo_native=*/true}},
    // Time-based interpolators — share Opcode::INTERP_TIME, dispatched by
    // inst_rate (0..3). Stereo-native opcodes with Match output: mono target
    // → mono output (slots into downstream mono params like saw(freq=...)
    // without E186); stereo target → independent per-channel ramps.
    {"interp",          {cedar::Opcode::INTERP_TIME, 2, 0, true,
                         {"target", "time", "", "", "", ""},
                         {NAN, NAN, NAN},
                         "Time-based interpolator (linear) — output width matches input",
                         0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/0}},
    {"interp_ease_in",  {cedar::Opcode::INTERP_TIME, 2, 0, true,
                         {"target", "time", "", "", "", ""},
                         {NAN, NAN, NAN},
                         "Time-based interpolator (ease-in, t²) — output width matches input",
                         0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/1}},
    {"interp_ease_out", {cedar::Opcode::INTERP_TIME, 2, 0, true,
                         {"target", "time", "", "", "", ""},
                         {NAN, NAN, NAN},
                         "Time-based interpolator (ease-out, 1-(1-t)²) — output width matches input",
                         0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/2}},
    {"interp_cos",      {cedar::Opcode::INTERP_TIME, 2, 0, true,
                         {"target", "time", "", "", "", ""},
                         {NAN, NAN, NAN},
                         "Time-based interpolator (cosine S-curve) — output width matches input",
                         0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/3}},
    // Edge primitives — share Opcode::EDGE_OP, dispatched by inst_rate (0..3).
    // Stereo-native opcodes with Match output: mono primary → mono output
    // (lets the result feed mono-only slots downstream); stereo primary →
    // independent per-channel hold/edge/counter state.
    {"sah",      {cedar::Opcode::EDGE_OP, 2, 0, true,
                 {"in", "trig", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Sample and hold — output width matches input",
                 0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/0}},
    {"gateup",   {cedar::Opcode::EDGE_OP, 1, 0, true,
                 {"sig", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "1.0 on rising edge of sig — output width matches input",
                 0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/1}},
    {"gatedown", {cedar::Opcode::EDGE_OP, 1, 0, true,
                 {"sig", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "1.0 on falling edge of sig — output width matches input",
                 0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/2}},
    {"counter",  {cedar::Opcode::EDGE_OP, 1, 2, true,
                 {"trig", "reset", "start", "", "", ""},
                 {NAN, NAN},
                 "Increment on rising edge of trig; reset to start (or 0) on rising edge of reset — output width matches input",
                 0, {}, {}, ChannelCount::Match, /*stereo_native=*/true, /*inst_rate=*/3}},

    // Output (1 required for mono, 2 for stereo)
    // out(...) is an alias for bus(0, ...) — the master bus (prd-bus-routing).
    {"out",     {.opcode = cedar::Opcode::OUTPUT,
                 .input_count = 1,
                 .optional_count = 1,
                 .requires_state = false,
                 .param_names = {"L", "R", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Audio output (mono or stereo) — alias for bus(0, ...)",
                 .extended_param_count = 0,
                 .param_types = {ParamValueType::Signal, ParamValueType::Signal},
                 .codegen_handler = BuiltinHandlers::handle_bus_call}},

    // Bus routing (prd-bus-routing Phase 1). bus(N, L, R?) sums a signal
    // into numbered bus N. Bus 0 is the master/device bus; every non-zero
    // bus auto-sums into bus 0. N must be a compile-time non-negative
    // integer literal. Handled entirely by handle_bus_call in codegen.
    // The optional trailing string is a friendly bus label (OQ4): it names the
    // stem file + mixer strip. bus(N, L, "kick") / bus(N, L, R, "kick").
    {"bus",     {.opcode = cedar::Opcode::OUTPUT,
                 .input_count = 2,
                 .optional_count = 2,
                 .requires_state = false,
                 .param_names = {"N", "L", "R", "label", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Route a signal into numbered bus N (bus 0 is the master)",
                 .codegen_handler = BuiltinHandlers::handle_bus_call}},

    // Per-bus FX (prd-bus-routing Phase 2). mixer(N, closure) attaches a
    // processing closure to bus N; master(closure) is an alias for
    // mixer(0, closure). NOP — both emit nothing at the call site; codegen
    // (handle_mixer_call) records the closure and inlines it into the bus
    // epilogue. The closure body runs once per block on the bus's summed
    // signal.
    // Both take an optional trailing string label (OQ4), same as bus().
    {"mixer",   {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 1,
                 .requires_state = false,
                 .param_names = {"N", "closure", "label", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Attach a processing closure to bus N",
                 .codegen_handler = BuiltinHandlers::handle_mixer_call}},
    {"master",  {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 1,
                 .requires_state = false,
                 .param_names = {"closure", "label", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Attach a processing closure to bus 0 — alias for "
                 "mixer(0, ...)",
                 .codegen_handler = BuiltinHandlers::handle_mixer_call}},

    // Stereo Operations (handled specially by codegen for stereo signal propagation)
    // stereo(mono) creates stereo from mono by duplicating to both channels
    // stereo(left, right) creates stereo from two separate signals
    {"stereo",  {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 1,
                 .requires_state = false,
                 .param_names = {"L", "R", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Create stereo signal from mono or L/R pair",
                 .extended_param_count = 0,
                 .param_types = {},
                 .input_channels = {},
                 .output_channels = ChannelCount::Stereo,
                 .codegen_handler = BuiltinHandlers::handle_stereo_call}},
    // Extract left channel from stereo signal
    {"left",    {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"stereo", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Extract left channel from stereo signal",
                 .extended_param_count = 0,
                 .param_types = {},
                 .input_channels = {ChannelCount::Stereo},
                 .output_channels = ChannelCount::Mono,
                 .codegen_handler = BuiltinHandlers::handle_left_call}},
    // Extract right channel from stereo signal
    {"right",   {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"stereo", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Extract right channel from stereo signal",
                 .extended_param_count = 0,
                 .param_types = {},
                 .input_channels = {ChannelCount::Stereo},
                 .output_channels = ChannelCount::Mono,
                 .codegen_handler = BuiltinHandlers::handle_right_call}},
    // Pan signal to stereo position. Same-arity overload: mono input emits PAN
    // (constant-power mono→stereo); stereo input emits PAN_STEREO (equal-power
    // DAW-style balance). Dispatch handled by handle_pan_call in codegen_stereo.cpp.
    {"pan",     {cedar::Opcode::PAN, 2, 0, false,
                 {"signal", "pos", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Pan signal at position (-1=L, 0=center, 1=R) — mono→stereo, or DAW-style stereo balance",
                 0, {}, {}, ChannelCount::Stereo}},
    // Stereo width control (0=mono, 1=normal, >1=wide)
    // Convenience: width(stereo, amount) or explicit: width(L, R, amount)
    {"width",   {.opcode = cedar::Opcode::WIDTH,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"stereo/L", "amount/R", "amount?", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Stereo width via M/S (0=mono, 1=normal, >1=wide) — width(stereo, amt) or width(L, R, amt)",
                 .extended_param_count = 0,
                 .param_types = {},
                 .input_channels = {ChannelCount::Stereo},
                 .output_channels = ChannelCount::Stereo,
                 .codegen_handler = BuiltinHandlers::handle_width_call}},
    // Mid/side encoding
    // Convenience: ms_encode(stereo) or explicit: ms_encode(L, R)
    {"ms_encode", {.opcode = cedar::Opcode::MS_ENCODE,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"stereo/L", "R?", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Encode stereo to mid/side — ms_encode(stereo) or ms_encode(L, R)",
                 .extended_param_count = 0,
                 .param_types = {},
                 .input_channels = {ChannelCount::Stereo},
                 .output_channels = ChannelCount::Stereo,
                 .codegen_handler = BuiltinHandlers::handle_ms_encode_call}},
    // Mid/side decoding
    // Convenience: ms_decode(ms) or explicit: ms_decode(M, S)
    {"ms_decode", {.opcode = cedar::Opcode::MS_DECODE,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"ms/M", "S?", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Decode mid/side to stereo — ms_decode(ms) or ms_decode(M, S)",
                 .extended_param_count = 0,
                 .param_types = {},
                 .input_channels = {ChannelCount::Stereo},
                 .output_channels = ChannelCount::Stereo,
                 .codegen_handler = BuiltinHandlers::handle_ms_decode_call}},
    // True stereo ping-pong delay
    // Convenience: pingpong(stereo, time, fb) or explicit: pingpong(L, R, time, fb, width?)
    // dry/wet ride in ExtendedParams<2> because all 5 input slots are used (L, R, time, fb, width).
    // optional_count=2 keeps find_param("dry")/("wet") at total_params() + ext_idx = {5, 6}, away
    // from the width slot — both width defaults are 1.0f and the custom handler picks the right one.
    {"pingpong", {.opcode = cedar::Opcode::DELAY_PINGPONG,
                  .input_count = 3, .optional_count = 2, .requires_state = true,
                  .param_names = {"stereo/L", "time/R", "fb/time", "fb?", "width?", ""},
                  .defaults = {1.0f, 1.0f, NAN, NAN, NAN},
                  .description = "Ping-pong stereo delay — pingpong(stereo, t, fb, width?) or pingpong(L, R, t, fb, width?)",
                  .extended_param_count = 2,
                  .input_channels = {ChannelCount::Stereo},
                  .output_channels = ChannelCount::Stereo,
                  .extended_param_names = {"dry", "wet"},
                  .extended_defaults = {1.0f, 0.5f}}},

    // Timing/Sequencing
    {"clock",   {cedar::Opcode::CLOCK,   0, 0, false,
                 {"", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Global clock signal"}},
    {"lfo",     {cedar::Opcode::LFO,     1, 1, true,
                 {"rate", "duty", "", "", "", ""},
                 {0.5f, NAN, NAN},
                 "Low frequency oscillator (-1 to 1)"}},
    {"trigger", {cedar::Opcode::TRIGGER, 1, 0, true,
                 {"div", "", "", "", "", ""},
                 {NAN, NAN, NAN},
                 "Clock divider trigger"}},
    {"euclid",  {cedar::Opcode::EUCLID,  2, 2, true,
                 {"hits", "steps", "rot", "dur", "", ""},
                 {0.0f, 4.0f, NAN},
                 "Euclidean rhythm trigger generator. dur = pattern span in cycles (default 4)."}},
    {"timeline", {.opcode = cedar::Opcode::TIMELINE,
                 .input_count = 0,
                 .optional_count = 1,
                 .requires_state = true,
                 .param_names = {"pattern", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Breakpoint automation timeline",
                 .codegen_handler = BuiltinHandlers::handle_timeline_call}},

    // Compile-time array functions (handled specially by codegen)
    {"len",     {.opcode = cedar::Opcode::PUSH_CONST,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"arr", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Array length (compile-time for static arrays, runtime for dynamic arrays)",
                 .codegen_handler = BuiltinHandlers::handle_len_call}},
    {"key_deltas", {.opcode = cedar::Opcode::PUSH_CONST,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"root", "intervals", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Compile-time nearest-tone delta table (12 entries) for a scale "
                 "given as root + semitone interval list; tie snaps lower. "
                 "Used by the user-defined key() overload (prd-scale-quantize §4.6)",
                 .codegen_handler = BuiltinHandlers::handle_key_deltas_call}},

    // Pattern-event chord accessors (PRD prd-pattern-event-arrays). Both are
    // dispatched via its codegen_handler; the opcode
    // here is a placeholder so the analyzer accepts the call. They take a
    // Pattern and return a DynArray of the active event's chord notes.
    // Not reserved — a user binding shadows them, exactly like len/map.
    {"notes",   {.opcode = cedar::Opcode::SEQPAT_VALUES,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = true,
                 .param_names = {"pattern", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Active pattern event's chord notes as a dynamic array of MIDI numbers",
                 .codegen_handler = BuiltinHandlers::handle_notes_call}},
    {"freqs",   {.opcode = cedar::Opcode::SEQPAT_VALUES,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = true,
                 .param_names = {"pattern", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Active pattern event's chord notes as a dynamic array of frequencies (Hz)",
                 .codegen_handler = BuiltinHandlers::handle_freqs_call}},

    // User state cells (Phase 3 of userspace-state PRD). All three are
    // dispatched via its codegen_handler; the opcode
    // here is just a placeholder so the analyzer accepts the call. Names
    // are reserved at the parser level — users cannot rebind them.
    {"state",   {.opcode = cedar::Opcode::STATE_OP,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = true,
                 .param_names = {"init", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Allocate a persistent state cell with the given initial value",
                 .codegen_handler = BuiltinHandlers::handle_state_call}},
    {"get",     {.opcode = cedar::Opcode::STATE_OP,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"cell", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Read the current value of a state cell",
                 .codegen_handler = BuiltinHandlers::handle_get_call}},
    {"set",     {.opcode = cedar::Opcode::STATE_OP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"cell", "value", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Write a value to a state cell, returns the new value",
                 .codegen_handler = BuiltinHandlers::handle_set_call}},

    // Multi-buffer array primitives for polyphony (handled specially by codegen)
    // These enable user-defined polyphony: fn poly(c, f) = sum(map(c, f)) / len(c)
    {"map",     {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"array", "fn", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Apply function to each element: map(array, (val) -> ...) or map(array, (val, idx) -> ...)",
                 .codegen_handler = BuiltinHandlers::handle_map_call}},
    // sum() is variadic and handled specially in the analyzer + codegen
    // (arity >= 1, not bounded by input_count/optional_count below).
    {"sum",     {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"array", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Sum signals per-channel, preserving stereo: sum(array) or sum(a, b, ...)",
                 .codegen_handler = BuiltinHandlers::handle_sum_call}},
    {"reduce",  {.opcode = cedar::Opcode::NOP,
                 .input_count = 3,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"array", "fn", "init", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Reduce array with binary function and initial value",
                 .codegen_handler = BuiltinHandlers::handle_reduce_call}},
    {"zipWith", {.opcode = cedar::Opcode::NOP,
                 .input_count = 3,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"a", "b", "fn", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Combine two arrays element-wise with binary function",
                 .codegen_handler = BuiltinHandlers::handle_zipWith_call}},
    {"zip",     {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"a", "b", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Interleave two arrays: [a0, b0, a1, b1, ...]",
                 .codegen_handler = BuiltinHandlers::handle_zip_call}},
    {"take",    {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"n", "array", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Take first n elements from array",
                 .codegen_handler = BuiltinHandlers::handle_take_call}},
    {"drop",    {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"n", "array", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Drop first n elements from array",
                 .codegen_handler = BuiltinHandlers::handle_drop_call}},
    {"reverse", {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"array", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Reverse array order",
                 .codegen_handler = BuiltinHandlers::handle_reverse_call}},
    {"range",   {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 1,
                 .requires_state = false,
                 .param_names = {"start", "end", "step", "", "", ""},
                 .defaults = {NAN, NAN, 1.0f},
                 .description = "Generate array [start, start±step, ...] toward end (exclusive); direction follows start/end, step defaults to 1",
                 .codegen_handler = BuiltinHandlers::handle_range_call}},
    {"repeat",  {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"value", "n", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Repeat value n times: [v, v, ..., v]",
                 .codegen_handler = BuiltinHandlers::handle_repeat_call}},

    // Array reduction operations
    {"mean",    {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"array", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Average of array elements",
                 .codegen_handler = BuiltinHandlers::handle_mean_call}},

    // Array transformation operations
    {"rotate",    {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"array", "n", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Rotate array elements by n positions",
                 .codegen_handler = BuiltinHandlers::handle_rotate_call}},
    {"shuffle",   {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 1,
                 .requires_state = false,
                 .param_names = {"array", "seed", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Deterministic random permutation of array; optional seed mixes into the path-derived seed",
                 .codegen_handler = BuiltinHandlers::handle_shuffle_call}},
    {"sort",      {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 1,
                 .requires_state = false,
                 .param_names = {"array", "reverse", "", "", "", ""},
                 .defaults = {NAN, 0.0f, NAN},
                 .description = "Sort array in ascending order; reverse=true sorts descending",
                 .codegen_handler = BuiltinHandlers::handle_sort_call}},
    {"normalize", {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 2,
                 .requires_state = false,
                 .param_names = {"array", "lo", "hi", "", "", ""},
                 .defaults = {NAN, 0.0f, 1.0f},
                 .description = "Scale array to [lo, hi] range (defaults to [0, 1])",
                 .codegen_handler = BuiltinHandlers::handle_normalize_call}},
    // Polyphony control. `release` (seconds) holds the voice in the mix
    // for that long past note-off so the instrument's own ADSR can finish
    // its release tail instead of being silenced by gate-multiplied mixing
    // — see PRD prd-midi-input §7.2. Default 0 = legacy behavior.
    {"poly",      {cedar::Opcode::NOP, 2, 2, true,
                   {"input", "instrument", "voices", "release", "", ""},
                   {64.0f, 0.0f, NAN, NAN, NAN},
                   "Polyphonic voice manager: allocates voices driven by a pattern input. Default 64 voices, max 128. `release` (seconds) extends per-voice mix tail past note-off.",
                   0, {}, {}, ChannelCount::Stereo, true}},
    // Higher-order DSL (PRD prd-runtime-functions-control-flow L3 §7.5). All
    // three compile to FOREACH_EVENT + a subprogram block; the lambda's
    // per-event parameter is an event record (n.freq / n.vel / n.gate / ...).
    // each_voice(input, lambda): runs the lambda once per event, mixing all
    // iterations into a stereo signal.
    {"each_voice", {cedar::Opcode::NOP, 2, 0, true,
                    {"input", "lambda", "", "", "", ""},
                    {NAN, NAN, NAN},
                    "Higher-order per-event instrument: each_voice(input, (n) -> ...) runs the lambda once per pattern event and mixes the outputs.",
                    0, {ParamValueType::Any, ParamValueType::Function},
                    {}, ChannelCount::Stereo, true}},
    // each(input, lambda): side-effecting per-event sink — the lambda body
    // calls out() itself; iterations accumulate into the global bus.
    {"each", {cedar::Opcode::NOP, 2, 0, true,
              {"input", "lambda", "", "", "", ""},
              {NAN, NAN, NAN},
              "Higher-order per-event sink: each(input, (n) -> ...) runs the lambda once per event for side effects (the body calls out() itself).",
              0, {ParamValueType::Any, ParamValueType::Function},
              {}, ChannelCount::Mono, true}},
    // Runtime event-stream transforms (PRD prd-runtime-event-transforms
    // Phase 2). event_map(events, (e) -> {...}) rewrites every event of a
    // pattern / MIDI stream via a closure returning a field-overlay record;
    // event_filter(events, (e) -> bool) drops events whose predicate is false.
    // Both compile to a closure EVENT_MAP / EVENT_FILTER opcode + a subprogram.
    {"event_map", {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = true,
                 .param_names = {"events", "transform", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Per-event rewrite: event_map(events, (e) -> {note: e.note + 7}) "
                   "transforms every event of a pattern or MIDI stream; the closure "
                   "returns a record whose fields overlay the event.",
                 .extended_param_count = 0,
                 .param_types = {},
                 .input_channels = {},
                 .output_channels = ChannelCount::Mono,
                 .stereo_native = false,
                 .codegen_handler = BuiltinHandlers::handle_event_map_call}},
    {"event_filter", {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = true,
                 .param_names = {"events", "predicate", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Per-event filter: event_filter(events, (e) -> e.vel > 0.5) "
                      "keeps only the events whose predicate closure is truthy.",
                 .extended_param_count = 0,
                 .param_types = {},
                 .input_channels = {},
                 .output_channels = ChannelCount::Mono,
                 .stereo_native = false,
                 .codegen_handler = BuiltinHandlers::handle_event_filter_call}},
    // Dual-role builtin: mono(stereo_signal) downmixes stereo→mono via (L+R)*0.5,
    // while mono(instrument) is the monophonic voice manager. The codegen
    // dispatcher routes based on argument type (see handle_mono_call).
    // The 2-arg voice-manager form `mono(input, instrument)` is also accepted
    // but the param_names below describe the 1-arg form; mixed positional +
    // named-arg use (e.g. `mono(synth, release: 0.3)`) is brittle for this
    // dual-role builtin — prefer fully-positional `mono(input, synth, 0.3)`.
    {"mono",      {cedar::Opcode::MONO_DOWNMIX, 1, 2, false,
                   {"signal_or_instrument", "input", "release", "", "", ""},
                   {NAN, 0.0f, NAN, NAN, NAN},
                   "Stereo-to-mono downmix (L+R)*0.5, or monophonic voice manager. `release` (seconds) extends mix tail past note-off in voice-manager mode.",
                   0, {}, {ChannelCount::Stereo}, ChannelCount::Mono}},
    {"legato",    {cedar::Opcode::NOP, 1, 2, false,
                   {"instrument", "input", "release", "", "", ""},
                   {NAN, 0.0f, NAN, NAN, NAN},
                   "Legato voice manager. `release` (seconds) extends mix tail past note-off."}},
    {"spread",    {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"n", "source", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Force source to specific voice count (pad/truncate)",
                 .codegen_handler = BuiltinHandlers::handle_spread_call}},

    // Array generation operations
    {"linspace",  {.opcode = cedar::Opcode::NOP,
                 .input_count = 3,
                 .optional_count = 1,
                 .requires_state = false,
                 .param_names = {"start", "end", "n", "mode", "", ""},
                 .defaults = {NAN, NAN, NAN, NAN},
                 .description = "Generate n evenly spaced values from start to end; mode=\"linear\" (default), \"log\", or \"geom\"",
                 .codegen_handler = BuiltinHandlers::handle_linspace_call}},
    {"random",    {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 2,
                 .requires_state = false,
                 .param_names = {"n", "min", "max", "", "", ""},
                 .defaults = {NAN, 0.0f, 1.0f},
                 .description = "Generate n random values in [min, max) (deterministic; defaults to [0, 1))",
                 .codegen_handler = BuiltinHandlers::handle_random_call}},
    {"harmonics", {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 1,
                 .requires_state = false,
                 .param_names = {"fundamental", "n", "ratio", "", "", ""},
                 .defaults = {NAN, NAN, 1.0f},
                 .description = "Generate harmonic series; ratio>1 stretches partials (piano-like inharmonicity), <1 compresses",
                 .codegen_handler = BuiltinHandlers::handle_harmonics_call}},

    // Function composition (handled specially by codegen)
    {"compose",   {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"f", "g", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Compose functions: compose(f, g)(x) = g(f(x))",
                 .codegen_handler = BuiltinHandlers::handle_compose_call}},

    // Chord function (handled specially by codegen)
    // chord("Am") -> array of MIDI notes (root note only for now)
    // chord("Am C7 F G") -> pattern of chord progressions
    {"chord",   {.opcode = cedar::Opcode::PUSH_CONST,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"symbol", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Chord expansion (Am, C7, Fmaj7, etc.)",
                 .codegen_handler = BuiltinHandlers::handle_chord_call}},

    // scalar(p) is the explicit Pattern→Signal cast.
    {"scalar",  {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Cast a note/value/chord pattern to its primary value buffer as a Signal.",
                 .codegen_handler = BuiltinHandlers::handle_scalar_call}},
    // Pattern transformation builtins (handled specially by codegen).
    // Phase 3 (prd-runtime-event-transforms): fast/slow lower to a runtime
    // EVENT_RATE_SCALE opcode that feeds the upstream SEQPAT_QUERY's
    // external-clock input. Factor accepts constants OR signal-rate
    // buffers (e.g. `n"c d e".fast(sine(0.2) + 2)`).
    {"slow",    {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "factor", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Slow down pattern by factor (stretch time). Factor may be a "
                 "constant or a signal; for live MIDI streams this is a "
                 "warned no-op.",
                 .codegen_handler = BuiltinHandlers::handle_slow_call}},
    {"fast",    {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "factor", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Speed up pattern by factor (compress time). Factor may be a "
                 "constant or a signal; for live MIDI streams this is a "
                 "warned no-op.",
                 .codegen_handler = BuiltinHandlers::handle_fast_call}},
    {"rev",     {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Reverse pattern event order.",
                 .codegen_handler = BuiltinHandlers::handle_rev_call}},
    {"transport", {cedar::Opcode::SEQPAT_TRANSPORT, 2, 2, true,
                 {"pattern", "trig", "step", "reset", "", ""},
                 {1.0f, NAN, NAN, NAN, NAN},
                 "Trigger-driven pattern transport — decouples a pattern from "
                 "the global clock; each trigger edge advances playback.",
                 0, {ParamValueType::Pattern}}},
    // transpose / velocity / dur / bend / aftertouch live in
    // akkado/stdlib/event_transforms.ak as one-line `event_map` wrappers
    // (prd-runtime-event-transforms Phase 2b).
    {"bank",    {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "bank_name", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Set sample bank for pattern events.",
                 .codegen_handler = BuiltinHandlers::handle_bank_call}},
    {"variant",  {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "index", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Set sample variant for pattern events.",
                 .codegen_handler = BuiltinHandlers::handle_variant_call}},
    {"tune",    {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"tuning", "pattern", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Apply microtonal tuning context to a pattern.",
                 .codegen_handler = BuiltinHandlers::handle_tune_call}},

    // Phase 2 PRD: time & structure modifiers (Strudel-compatible).
    // All compile-time event-list rewrites; opcode is NOP.
    // early / late live in akkado/stdlib/event_transforms.ak (Phase 2b).
    {"palindrome", {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Play pattern forward then reversed (doubles cycle length).",
                 .codegen_handler = BuiltinHandlers::handle_palindrome_call}},
    {"compress",   {.opcode = cedar::Opcode::NOP,
                 .input_count = 3,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "start", "end", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Squash pattern into [start, end) of cycle (silence elsewhere).",
                 .codegen_handler = BuiltinHandlers::handle_compress_call}},
    {"ply",        {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "n", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Repeat each event n times within its slot.",
                 .codegen_handler = BuiltinHandlers::handle_ply_call}},
    {"linger",     {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "frac", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Keep first frac of pattern; loop it to fill the cycle.",
                 .codegen_handler = BuiltinHandlers::handle_linger_call}},
    {"zoom",       {.opcode = cedar::Opcode::NOP,
                 .input_count = 3,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "start", "end", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Play only [start, end) portion of pattern, stretched to fill cycle.",
                 .codegen_handler = BuiltinHandlers::handle_zoom_call}},
    {"segment",    {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "n", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Sample pattern at n evenly-spaced points; emit n equal-duration events.",
                 .codegen_handler = BuiltinHandlers::handle_segment_call}},
    // swing / swingBy live in akkado/stdlib/event_transforms.ak (Phase 2b).
    {"iter",       {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "n", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Rotate pattern start by 1/n per cycle (forward).",
                 .codegen_handler = BuiltinHandlers::handle_iter_call}},
    {"iterBack",   {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "n", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Rotate pattern start by 1/n per cycle (backward).",
                 .codegen_handler = BuiltinHandlers::handle_iter_back_call}},

    // Phase 2 PRD: algorithmic pattern generators.
    // These emit a PatternEventStream directly (no inner pattern).
    {"run",        {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"n", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Pattern of integers 0..n-1 evenly distributed in cycle.",
                 .codegen_handler = BuiltinHandlers::handle_run_call}},
    {"binary",     {.opcode = cedar::Opcode::NOP,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"n", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Trigger pattern from binary representation of n (MSB first).",
                 .codegen_handler = BuiltinHandlers::handle_binary_call}},
    {"binaryN",    {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"n", "bits", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Trigger pattern from low `bits` bits of n (zero-padded, MSB first).",
                 .codegen_handler = BuiltinHandlers::handle_binary_n_call}},

    // Phase 2 PRD: voicing transforms (chord-event manipulation).
    {"anchor",     {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "note", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Set anchor MIDI note for chord voicing (e.g., \"c4\").",
                 .codegen_handler = BuiltinHandlers::handle_anchor_call}},
    {"mode",       {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "mode", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Set chord voicing mode: below/above/duck/root.",
                 .codegen_handler = BuiltinHandlers::handle_mode_call}},
    {"voicing",    {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"pattern", "name", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Apply named voicing dictionary (close/open/drop2/drop3 or custom).",
                 .codegen_handler = BuiltinHandlers::handle_voicing_call}},
    {"addVoicings", {.opcode = cedar::Opcode::NOP,
                 .input_count = 2,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"name", "intervals", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Register a custom voicing dictionary by name.",
                 .codegen_handler = BuiltinHandlers::handle_add_voicings_call}},

    // Parameter exposure builtins (handled specially by codegen)
    // These extract metadata at compile time for UI generation
    {"param",   {.opcode = cedar::Opcode::ENV_GET,
                 .input_count = 2,
                 .optional_count = 2,
                 .requires_state = false,
                 .param_names = {"name", "default", "min", "max", "", ""},
                 .defaults = {NAN, 0.0f, 0.0f, 1.0f},
                 .description = "Continuous parameter (slider). Reads from EnvMap.",
                 .codegen_handler = BuiltinHandlers::handle_param_call}},
    {"button",  {.opcode = cedar::Opcode::ENV_GET,
                 .input_count = 1,
                 .optional_count = 0,
                 .requires_state = false,
                 .param_names = {"name", "", "", "", "", ""},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Momentary button. 1 while pressed, 0 otherwise.",
                 .codegen_handler = BuiltinHandlers::handle_button_call}},
    {"toggle",  {.opcode = cedar::Opcode::ENV_GET,
                 .input_count = 1,
                 .optional_count = 1,
                 .requires_state = false,
                 .param_names = {"name", "default", "", "", "", ""},
                 .defaults = {NAN, 0.0f, NAN},
                 .description = "Boolean toggle. Click to flip between 0 and 1.",
                 .codegen_handler = BuiltinHandlers::handle_toggle_call}},
    {"dropdown", {.opcode = cedar::Opcode::ENV_GET,
                 .input_count = 2,
                 .optional_count = 6,
                 .requires_state = false,
                 .param_names = {"name", "opt1", "opt2", "opt3", "opt4", "opt5"},
                 .defaults = {NAN, NAN, NAN},
                 .description = "Selection dropdown. Returns index (0, 1, ...) of selected option.",
                 .codegen_handler = BuiltinHandlers::handle_select_call}},

    // Visualization builtins (handled specially by codegen)
    // These create visualization widgets in the editor and pass signal through
    // Signature: viz(signal, name?, options?) where options is a record {width, height, ...}
    // Option-field schemas are declared in viz_schemas (see further below in this header).
    {"pianoroll", {.opcode = cedar::Opcode::COPY, .input_count = 1, .optional_count = 2, .requires_state = false,
                   .param_names = {"signal", "name", "options", "", "", ""},
                   .defaults = {NAN, NAN, NAN, NAN, NAN},
                   .description = "Attach piano roll visualization. Signal passes through unchanged.",
                   .param_types = {ParamValueType::Signal, ParamValueType::String, ParamValueType::Record},
                   .option_schemas = {OptionSchema{
                       /*param_index=*/2,
                       /*fields=*/{{
                           {"width",    OptionFieldType::Number, "200",        "Canvas width (pixels, or '%' string for relative)"},
                           {"height",   OptionFieldType::Number, "50",         "Canvas height (pixels, or '%' string for relative)"},
                           {"beats",    OptionFieldType::Number, "",           "Window duration in beats (defaults to cycle length)"},
                           {"showGrid", OptionFieldType::Bool,   "true",       "Display vertical beat grid"},
                           {"scale",    OptionFieldType::Enum,   "\"chromatic\"", "MIDI scale filter", "chromatic,pentatonic,octave"},
                       }},
                       /*field_count=*/5,
                   }},
                   .option_schema_count = 1,
                 .codegen_handler = BuiltinHandlers::handle_pianoroll_call}},
    {"oscilloscope", {.opcode = cedar::Opcode::COPY, .input_count = 1, .optional_count = 2, .requires_state = true,
                      .param_names = {"signal", "name", "options", "", "", ""},
                      .defaults = {NAN, NAN, NAN, NAN, NAN},
                      .description = "Attach oscilloscope visualization. Signal passes through unchanged.",
                      .param_types = {ParamValueType::Signal, ParamValueType::String, ParamValueType::Record},
                      .option_schemas = {OptionSchema{
                          /*param_index=*/2,
                          /*fields=*/{{
                              {"width",        OptionFieldType::Number, "200",       "Canvas width (pixels, or '%' string for relative)"},
                              {"height",       OptionFieldType::Number, "50",        "Canvas height (pixels, or '%' string for relative)"},
                              {"triggerLevel", OptionFieldType::Number, "0",         "Trigger threshold (signal value)"},
                              {"triggerEdge",  OptionFieldType::Enum,   "\"rising\"", "Trigger direction", "rising,falling"},
                          }},
                          /*field_count=*/4,
                      }},
                      .option_schema_count = 1,
                 .codegen_handler = BuiltinHandlers::handle_oscilloscope_call}},
    {"waveform", {.opcode = cedar::Opcode::COPY, .input_count = 1, .optional_count = 2, .requires_state = true,
                  .param_names = {"signal", "name", "options", "", "", ""},
                  .defaults = {NAN, NAN, NAN, NAN, NAN},
                  .description = "Attach waveform visualization. Signal passes through unchanged.",
                  .param_types = {ParamValueType::Signal, ParamValueType::String, ParamValueType::Record},
                  .option_schemas = {OptionSchema{
                      /*param_index=*/2,
                      /*fields=*/{{
                          {"width",    OptionFieldType::Number, "200",  "Canvas width (pixels, or '%' string for relative)"},
                          {"height",   OptionFieldType::Number, "50",   "Canvas height (pixels, or '%' string for relative)"},
                          {"scale",    OptionFieldType::Number, "1.0",  "Amplitude scaling factor"},
                          {"filled",   OptionFieldType::Bool,   "true", "Use filled envelope vs outline"},
                          {"duration", OptionFieldType::Number, "5",    "Time window in seconds"},
                      }},
                      /*field_count=*/5,
                  }},
                  .option_schema_count = 1,
                 .codegen_handler = BuiltinHandlers::handle_waveform_call}},
    {"spectrum", {.opcode = cedar::Opcode::COPY, .input_count = 1, .optional_count = 2, .requires_state = true,
                  .param_names = {"signal", "name", "options", "", "", ""},
                  .defaults = {NAN, NAN, NAN, NAN, NAN},
                  .description = "Attach spectrum analyzer visualization. Signal passes through unchanged.",
                  .param_types = {ParamValueType::Signal, ParamValueType::String, ParamValueType::Record},
                  .option_schemas = {OptionSchema{
                      /*param_index=*/2,
                      /*fields=*/{{
                          {"width",    OptionFieldType::Number, "200",   "Canvas width (pixels, or '%' string for relative)"},
                          {"height",   OptionFieldType::Number, "50",    "Canvas height (pixels, or '%' string for relative)"},
                          {"logScale", OptionFieldType::Bool,   "false", "Log frequency axis vs linear"},
                          {"minDb",    OptionFieldType::Number, "-90",   "Lower dB limit for normalization"},
                          {"maxDb",    OptionFieldType::Number, "0",     "Upper dB limit for normalization"},
                          {"fft",      OptionFieldType::Enum,   "1024",  "FFT bin count", "256,512,1024,2048"},
                      }},
                      /*field_count=*/6,
                  }},
                  .option_schema_count = 1,
                 .codegen_handler = BuiltinHandlers::handle_spectrum_call}},
    {"waterfall", {.opcode = cedar::Opcode::COPY, .input_count = 1, .optional_count = 2, .requires_state = true,
                    .param_names = {"signal", "name", "options", "", "", ""},
                    .defaults = {NAN, NAN, NAN, NAN, NAN},
                    .description = "Attach scrolling spectrogram visualization. Signal passes through unchanged.",
                    .param_types = {ParamValueType::Signal, ParamValueType::String, ParamValueType::Record},
                    .option_schemas = {OptionSchema{
                        /*param_index=*/2,
                        /*fields=*/{{
                            {"width",    OptionFieldType::Number, "300",      "Canvas width (pixels, or '%' string for relative)"},
                            {"height",   OptionFieldType::Number, "150",      "Canvas height (pixels, or '%' string for relative)"},
                            {"angle",    OptionFieldType::Number, "180",      "Scroll direction in degrees (0–360)"},
                            {"speed",    OptionFieldType::Number, "40",       "Scroll speed in pixels/sec"},
                            {"fft",      OptionFieldType::Enum,   "1024",     "FFT bin count", "256,512,1024,2048"},
                            {"gradient", OptionFieldType::Enum,   "\"magma\"", "Color gradient preset", "magma,viridis,inferno,grayscale"},
                            {"minDb",    OptionFieldType::Number, "-90",      "Lower dB limit for normalization"},
                            {"maxDb",    OptionFieldType::Number, "0",        "Upper dB limit for normalization"},
                        }},
                        /*field_count=*/8,
                    }},
                    .option_schema_count = 1,
                 .codegen_handler = BuiltinHandlers::handle_waterfall_call}},

    // Builtin variable getters (desugared from identifier reads like `bpm`, `spb`)
    // These are registered in BUILTIN_FUNCTIONS so the analyzer accepts them as builtins.
    // The identifier path in codegen.cpp:433 handles the actual desugaring to ENV_GET.
    {"get_bpm", {cedar::Opcode::ENV_GET, 0, 0, false,
                  {"", "", "", "", "", ""},
                  {NAN, NAN, NAN, NAN, NAN},
                  "Get current BPM (beats per minute)."}},
    {"get_sr",  {cedar::Opcode::ENV_GET, 0, 0, false,
                  {"", "", "", "", "", ""},
                  {NAN, NAN, NAN, NAN, NAN},
                  "Get current sample rate."}},
    {"get_spb", {cedar::Opcode::ENV_GET, 0, 0, false,
                  {"", "", "", "", "", ""},
                  {NAN, NAN, NAN, NAN, NAN},
                  "Get seconds per beat = 60.0 / BPM."}},
});

/// Alias mappings for convenience syntax
/// e.g., "sine" -> "sin", "lowpass" -> "lp"
inline constexpr auto BUILTIN_ALIASES = frozen::make_unordered_map<frozen::string, std::string_view>({
    {"lowpass",   "lp"},
    {"highpass",  "hp"},
    {"bandpass",  "bp"},
    {"output",    "out"},
    {"moogladder", "moog"},
    {"envelope",  "adsr"},
    {"envfollow", "env_follower"},
    {"follower",  "env_follower"},
    // SVF aliases with explicit naming
    {"svflp",     "lp"},
    {"svfhp",     "hp"},
    {"svfbp",     "bp"},
    // SquelchEngine filter aliases
    {"diodeladder", "diode"},
    {"tb303",       "diode"},
    {"acid",        "diode"},
    {"vowel",       "formant"},
    {"sk",          "sallenkey"},
    {"ms20",        "sallenkey"},
    // Reverb aliases
    {"reverb",    "freeverb"},
    {"plate",     "dattorro"},
    {"room",      "fdn"},
    // Distortion aliases
    // Note: tanh(x) is now a pure math function
    // Use saturate(in, drive) for the saturation effect
    {"distort",   "saturate"},
    {"crush",     "bitcrush"},
    {"wavefold",  "fold"},
    {"valve",     "tube"},
    {"triode",    "tube"},
    {"adaa",      "smooth"},
    {"transformer", "xfmr"},
    {"console",   "xfmr"},
    {"exciter",   "excite"},
    {"aural",     "excite"},
    // Dynamics aliases
    // NOTE: `compress` was previously aliased to `comp` (audio compressor) but
    // is now reserved for the Strudel-style pattern transform. Audio users
    // must use `comp(...)` or `compressor(...)`.
    {"compressor", "comp"},
    {"limit",     "limiter"},
    {"noisegate", "gate"},
});

#ifdef CEDAR_HOST_EXTENSIONS
// Defined in host_extensions.cpp. Declared here (rather than including that
// header) to keep builtins.hpp free of any dependency on the registry.
const BuiltinInfo* lookup_host_builtin(std::string_view name);
#endif

/// Lookup a builtin by name, handling aliases
/// Returns nullptr if not found
inline const BuiltinInfo* lookup_builtin(std::string_view name) {
    // Check for alias first
    auto alias_it = BUILTIN_ALIASES.find(name);
    if (alias_it != BUILTIN_ALIASES.end()) {
        name = alias_it->second;
    }

    // Lookup in main table
    auto it = BUILTIN_FUNCTIONS.find(name);
    if (it != BUILTIN_FUNCTIONS.end()) {
        return &it->second;
    }
#ifdef CEDAR_HOST_EXTENSIONS
    // Miss-path only: host names can never shadow a core name (collisions are
    // rejected at registration), so the ordering here is pure optimization.
    if (const BuiltinInfo* host = lookup_host_builtin(name)) {
        return host;
    }
#endif
    return nullptr;
}

/// Get the canonical name for a function (resolves aliases)
inline std::string_view canonical_name(std::string_view name) {
    auto alias_it = BUILTIN_ALIASES.find(name);
    if (alias_it != BUILTIN_ALIASES.end()) {
        return alias_it->second;
    }
    return name;
}

// ============================================================================
// Builtin Variables (bpm, sr)
// ============================================================================

/// Definition of a builtin variable that desugars to ENV_GET reads and
/// compile-time constant extraction for writes.
// BuiltinVarDef lives in builtin_info.hpp (host_extensions.hpp needs it
// without pulling in this table header).

inline constexpr auto BUILTIN_VARIABLES = frozen::make_unordered_map<frozen::string, BuiltinVarDef>({
    {"bpm", {"get_bpm", "set_bpm", "__bpm", 120.0f, 1.0f, 999.0f}},
    {"sr",  {"get_sr",  "",         "__sr",  48000.0f, 0.0f, 0.0f}},
    {"spb", {"get_spb", "",         "__spb", 0.5f, 0.0f, 0.0f}},  // seconds per beat = 60.0 / bpm
});

#ifdef CEDAR_HOST_EXTENSIONS
// Defined in host_extensions.cpp — see lookup_host_builtin above.
const BuiltinVarDef* lookup_host_variable(std::string_view name);
#endif

/// Lookup a builtin variable (`bpm`, `sr`, `spb`), falling back to the host
/// registry. Returns nullptr if the name is not a builtin variable.
inline const BuiltinVarDef* lookup_builtin_variable(std::string_view name) {
    auto it = BUILTIN_VARIABLES.find(name);
    if (it != BUILTIN_VARIABLES.end()) {
        return &it->second;
    }
#ifdef CEDAR_HOST_EXTENSIONS
    if (const BuiltinVarDef* host = lookup_host_variable(name)) {
        return host;
    }
#endif
    return nullptr;
}

} // namespace akkado
