#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <algorithm>
#include <variant>
#include <cstring>
#include <new>
#include "../vm/audio_arena.hpp"
#include "sequence.hpp"

namespace cedar {

// Oscillator state - maintains phase for continuity
struct OscState {
    float phase = 0.0f;       // 0.0 to 1.0
    float prev_phase = 0.0f;  // Previous phase for PolyBLEP discontinuity detection
    float prev_trigger = 0.0f; // Previous trigger value for rising edge detection
    bool initialized = false; // Skip anti-aliasing on first sample
};

// Wavetable oscillator state (Smooch). Phase kept in double for cleanly
// accumulating drift over long renders and across hot-swap boundaries; the
// per-sample read still does float math via narrowing once per sample.
//
// Two cascaded 1-pole filters on the tablePos input give -12 dB/octave
// rolloff so the stair-step output of the EnvMap (param-system slew) at
// the UI's per-tick rate (~60 Hz) doesn't bleed audible sidebands into
// the morph crossfade. See op_osc_wavetable for the exact tuning.
struct SmoochState {
    double phase = 0.0;       // [0, 1) phase accumulator
    float  pos_smoothed_1 = 0.0f;
    float  pos_smoothed_2 = 0.0f;
    bool   initialized = false;
    bool   pos_initialized = false;
};

#ifndef CEDAR_NO_MINBLEP
// MinBLEP oscillator state with residual buffer
struct MinBLEPOscState {
    float phase = 0.0f;
    float prev_trigger = 0.0f; // Previous trigger value for rising edge detection
    bool initialized = false;

    // MinBLEP residual buffer (128 samples should be plenty)
    static constexpr std::size_t BUFFER_SIZE = 128;
    float buffer[BUFFER_SIZE] = {};
    std::size_t write_pos = 0;

    // Add a step discontinuity to the buffer
    void add_step(float amplitude, float frac_pos, const float* minblep_table,
                  std::size_t table_phases, std::size_t samples_per_phase);

    // Get current sample's residual and advance
    float get_and_advance() {
        float value = buffer[write_pos];
        buffer[write_pos] = 0.0f;
        write_pos = (write_pos + 1) % BUFFER_SIZE;
        return value;
    }

    void reset() {
        std::fill_n(buffer, BUFFER_SIZE, 0.0f);
        write_pos = 0;
        phase = 0.0f;
        initialized = false;
    }
};
#endif // CEDAR_NO_MINBLEP

// Oversampled oscillator state (4x)
// For even cleaner FM synthesis at high modulation indices
struct OscState4x {
    OscState osc;           // Base oscillator state

    // Simple averaging downsample 4x -> 1x
    float downsample(float s0, float s1, float s2, float s3) {
        return (s0 + s1 + s2 + s3) * 0.25f;
    }
};

// SVF (State Variable Filter) state — stereo-native (prd-stereo-native-opcodes
// Phase 4a). Integrator memory is per-channel [0]=L, [1]=R; coefficient cache
// is shared because freq/q are mono control signals.
struct SVFState {
    float ic1eq[2] = {0.0f, 0.0f};
    float ic2eq[2] = {0.0f, 0.0f};

    // Cached coefficients (shared across channels — driven by mono ctrl freq/q)
    float g = 0.0f;
    float k = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float a3 = 0.0f;

    // Last parameters
    float last_freq = -1.0f;
    float last_q = -1.0f;
};

// Noise generator state (LCG for deterministic noise)
// Supports frequency-controlled sample-and-hold mode
struct NoiseState {
    std::uint32_t seed = 12345;         // Current RNG state
    std::uint32_t start_seed = 12345;   // Initial seed (reset target)
    float phase = 0.0f;                 // Phase accumulator for freq>0 mode
    float prev_trigger = 0.0f;          // For trigger detection
    float current_value = 0.0f;         // Held sample value
    bool initialized = false;           // First-time init flag
};

// Slew rate limiter state
struct SlewState {
    float current = 0.0f;
    bool initialized = false;
};

// Edge-detection / sample-and-hold / counter state (EDGE_OP, all modes)
//   prev_reset_trigger is only used in counter mode (rate=3)
struct EdgeState {
    float held_value = 0.0f;
    float prev_trigger = 0.0f;
    float prev_reset_trigger = 0.0f;
};

// User-cell state (STATE_OP) — single float slot for state(init)/.get()/.set()
struct CellState {
    float value = 0.0f;
    bool initialized = false;
};

// Delay state with arena-allocated ring buffer
// Buffer is allocated from AudioArena on first use (zero heap allocation during audio)
struct DelayState {
    // Maximum delay time: 10 seconds at 96kHz = 960000 samples
    static constexpr std::size_t MAX_DELAY_SAMPLES = 960000;

    // Ring buffer (allocated from arena)
    float* buffer = nullptr;
    std::size_t buffer_size = 0;    // Allocated size in floats
    std::size_t write_pos = 0;

    // Smoothed delay time (prevents clicks when delay time changes)
    float smoothed_delay = 0.0f;       // Current smoothed delay time in samples
    bool delay_initialized = false;    // For first-sample initialization

    // Tap delay coordination (used by DELAY_TAP/DELAY_WRITE pair)
    // Caches delayed samples between TAP and WRITE so feedback chain can process them
    static constexpr std::size_t BLOCK_SIZE = 128;
    std::array<float, BLOCK_SIZE> tap_cache{};

    // Ensure buffer is allocated with requested size
    // arena: AudioArena to allocate from (from ExecutionContext)
    void ensure_buffer(std::size_t samples, AudioArena* arena) {
        if (buffer && buffer_size >= samples) {
            return;  // Already have enough space
        }
        if (!arena) return;

        std::size_t new_size = std::min(samples, MAX_DELAY_SAMPLES);
        float* new_buffer = arena->allocate(new_size);
        if (new_buffer) {
            buffer = new_buffer;
            buffer_size = new_size;
            write_pos = 0;
        }
    }

    // Reset buffer to silence (for seek)
    void reset() {
        if (buffer && buffer_size > 0) {
            std::memset(buffer, 0, buffer_size * sizeof(float));
            write_pos = 0;
        }
        tap_cache.fill(0.0f);
        smoothed_delay = 0.0f;
        delay_initialized = false;
    }
};

// Envelope state for ADSR
struct EnvState {
    float level = 0.0f;
    std::uint8_t stage = 0;     // 0=idle, 1=attack, 2=decay, 3=sustain, 4=release
    float time_in_stage = 0.0f;
    float prev_gate = 0.0f;     // For gate edge detection
    float release_level = 0.0f; // Level when release triggered (for smooth release)
    bool release_pending = false; // True if gate went off during attack/decay

    // Cached exponential coefficients for each stage
    float attack_coeff = 0.0f;
    float decay_coeff = 0.0f;
    float release_coeff = 0.0f;

    // Cached parameters for coefficient invalidation
    float last_attack = -1.0f;
    float last_decay = -1.0f;
    float last_release = -1.0f;
};

// ============================================================================
// Sequencing & Timing States
// ============================================================================

// LFO state - beat-synced low frequency oscillator
struct LFOState {
    float prev_phase = 1.0f;   // Previous phase for S&H wrap detection (init to 1.0 to detect first wrap)
    float prev_value = 0.0f;   // For SAH mode (last sampled value)
};

// Euclidean rhythm generator state
struct EuclidState {
    std::uint32_t prev_step = UINT32_MAX;  // Previous step for change detection (UINT32_MAX = uninitialized)

    // Precomputed pattern as bitmask (1 = trigger, 0 = rest)
    std::uint32_t pattern = 0;

    // Cached parameters for invalidation
    std::uint32_t last_hits = 0;
    std::uint32_t last_steps = 0;
    std::uint32_t last_rotation = 0;
};

// Trigger/impulse generator state
struct TriggerState {
    float prev_phase = 1.0f;  // Previous phase for wrap detection (init to 1.0 to detect first trigger)
};

// Transport state for trigger-driven pattern clock
struct TransportState {
    float beat_pos = 0.0f;
    float cycle_length = 4.0f;
    float last_trig = 0.0f;
    float last_reset = 0.0f;
};

// Timeline/breakpoint automation state
struct TimelineState {
    static constexpr std::size_t MAX_BREAKPOINTS = 64;

    struct Breakpoint {
        float time = 0.0f;      // Time in beats
        float value = 0.0f;     // Target value
        std::uint8_t curve = 0; // 0=linear, 1=exponential, 2=hold
    };

    Breakpoint points[MAX_BREAKPOINTS] = {};
    std::uint32_t num_points = 0;
    bool loop = false;
    float loop_length = 0.0f;  // Loop length in beats (0 = no loop)
};

// Moog-style 4-pole ladder filter state — stereo-native
// (prd-stereo-native-opcodes Phase 4a). Per-channel stage/delay arrays;
// coefficients shared across channels (mono ctrl freq/res).
struct MoogState {
    // 4 cascaded 1-pole lowpass stages per channel [ch][stage]
    float stage[2][4] = {};
    float delay[2][4] = {};  // Unit delays for trapezoidal integration

    // Cached parameters for coefficient invalidation
    float last_freq = -1.0f;
    float last_res = -1.0f;

    // Cached coefficients (shared across channels)
    float g = 0.0f;  // Cutoff coefficient (tan-based)
    float k = 0.0f;  // Resonance coefficient (0-4 range)
};

// ZDF Diode ladder filter state (TB-303 acid) — stereo-native
// (prd-stereo-native-opcodes Phase 4a). Per-channel capacitor states;
// coefficients shared.
struct DiodeState {
    // 4 capacitor states for cascade stages, per channel [ch][stage]
    float cap[2][4] = {};

    // Cached parameters for coefficient invalidation
    float last_freq = -1.0f;
    float last_res = -1.0f;

    // Cached coefficients (shared across channels)
    float g = 0.0f;  // Cutoff coefficient
    float k = 0.0f;  // Resonance coefficient (0-4)
};

// Formant (vowel) filter state - 3 parallel bandpass filters — stereo-native
// (prd-stereo-native-opcodes Phase 4a). Per-channel biquad memory;
// vowel-derived formant targets and gains are shared (mono ctrl).
struct FormantState {
    // 3 Chamberlin SVF bandpass filter states (2 integrators each), per channel
    float bp1_z1[2] = {0.0f, 0.0f}, bp1_z2[2] = {0.0f, 0.0f};
    float bp2_z1[2] = {0.0f, 0.0f}, bp2_z2[2] = {0.0f, 0.0f};
    float bp3_z1[2] = {0.0f, 0.0f}, bp3_z2[2] = {0.0f, 0.0f};

    // Cached parameters for coefficient invalidation
    float last_morph = -1.0f;
    float last_vowel_a = -1.0f;
    float last_vowel_b = -1.0f;
    float last_q = -1.0f;

    // Cached target frequencies (interpolated between vowels)
    float f1 = 650.0f, f2 = 1100.0f, f3 = 2860.0f;
    // Cached gains per formant
    float g1 = 1.0f, g2 = 0.5f, g3 = 0.25f;
};

// Sallen-Key (MS-20 style) filter state — stereo-native
// (prd-stereo-native-opcodes Phase 4a). Per-channel capacitor & diode-clip
// memory; coefficients shared.
struct SallenkeyState {
    // 2 capacitor states per channel [0]=L, [1]=R
    float cap1[2] = {0.0f, 0.0f};
    float cap2[2] = {0.0f, 0.0f};

    // Diode clipper state in feedback path, per channel
    float diode_state[2] = {0.0f, 0.0f};

    // Cached parameters
    float last_freq = -1.0f;
    float last_res = -1.0f;

    // Cached coefficients (shared across channels)
    float g = 0.0f;
    float k = 0.0f;
};

// ADAA wavefolder state — stereo-native (prd-stereo-native-opcodes Phase 4b).
struct FoldADAAState {
    float x_prev[2] = {0.0f, 0.0f};   // Previous input sample, per channel
    float ad_prev[2] = {0.0f, 0.0f};  // Previous antiderivative value, per channel
};

// ============================================================================
// Distortion States — all stereo-native (prd-stereo-native-opcodes Phase 4b).
// Per-channel memory inside one state struct; runtime parameters (drive,
// threshold, frequency, etc.) are mono control signals shared across L/R.
// ============================================================================

// Bitcrusher state (sample rate reduction)
struct BitcrushState {
    float held_sample[2] = {0.0f, 0.0f};
    float phase[2] = {0.0f, 0.0f};
};

// ADAA (Antiderivative Antialiasing) saturation state
// Used for alias-free tanh saturation without oversampling
struct SmoothSatState {
    float x_prev[2] = {0.0f, 0.0f};      // Previous input sample
    float ad_prev[2] = {0.0f, 0.0f};     // Previous antiderivative value F₁(x_prev)
    bool initialized[2] = {false, false};  // First-sample direct-tanh guard
};

// Tube saturation state (with oversampling)
struct TubeState {
    // 2x oversampling delay lines, per channel
    float os_delay[2][4] = {};
    int os_idx[2] = {0, 0};
};

// Tape saturation state (with oversampling and high-shelf filter)
struct TapeState {
    // 2x oversampling delay lines, per channel
    float os_delay[2][4] = {};
    int os_idx[2] = {0, 0};

    // High-shelf filter state for warmth control, per channel
    float hs_z1[2] = {0.0f, 0.0f};
};

// Transformer saturation state (bass-emphasis saturation)
struct XfmrState {
    // 2x oversampling delay lines, per channel
    float os_delay[2][4] = {};
    int os_idx[2] = {0, 0};

    // Leaky integrator for bass extraction, per channel
    float integrator[2] = {0.0f, 0.0f};
};

// Harmonic exciter state
struct ExciterState {
    // 2x oversampling delay lines, per channel
    float os_delay[2][4] = {};
    int os_idx[2] = {0, 0};

    // High-pass filter state (~3kHz), per channel
    float hp_z1[2] = {0.0f, 0.0f};
};

// ============================================================================
// Modulation Effect States
// ============================================================================

// Comb filter state with arena-allocated buffer
struct CombFilterState {
    static constexpr std::size_t MAX_COMB_SAMPLES = 4800;  // 100ms at 48kHz

    float* buffer = nullptr;
    std::size_t write_pos = 0;
    float filter_state = 0.0f;  // For damping lowpass

    void ensure_buffer(AudioArena* arena) {
        if (buffer) return;
        if (!arena) return;
        buffer = arena->allocate(MAX_COMB_SAMPLES);
    }

    void reset() {
        if (buffer) {
            std::memset(buffer, 0, MAX_COMB_SAMPLES * sizeof(float));
            write_pos = 0;
            filter_state = 0.0f;
        }
    }
};

// Flanger state with per-channel arena-allocated buffers (stereo-native).
// L/R lanes share a master LFO phase advanced once per sample; the R lane
// reads the LFO at base + 90° for stereo decorrelation. See
// prd-stereo-native-opcodes Phase 3.
struct FlangerState {
    static constexpr std::size_t MAX_FLANGER_SAMPLES = 960;  // 20ms at 48kHz

    float* buffer[2] = {nullptr, nullptr};   // [0] = L, [1] = R
    std::size_t write_pos[2] = {0, 0};
    float lfo_phase = 0.0f;

    void ensure_buffers(AudioArena* arena) {
        if (!arena) return;
        if (!buffer[0]) buffer[0] = arena->allocate(MAX_FLANGER_SAMPLES);
        if (!buffer[1]) buffer[1] = arena->allocate(MAX_FLANGER_SAMPLES);
    }

    void reset() {
        for (std::size_t ch = 0; ch < 2; ++ch) {
            if (buffer[ch]) {
                std::memset(buffer[ch], 0, MAX_FLANGER_SAMPLES * sizeof(float));
                write_pos[ch] = 0;
            }
        }
        lfo_phase = 0.0f;
    }
};

// Chorus state (multi-voice) with per-channel arena-allocated buffers
// (stereo-native). Master LFO phase shared; R lane reads at base + 90°.
struct ChorusState {
    static constexpr std::size_t MAX_CHORUS_SAMPLES = 2400;  // 50ms at 48kHz
    static constexpr std::size_t NUM_VOICES = 3;

    float* buffer[2] = {nullptr, nullptr};
    std::size_t write_pos[2] = {0, 0};
    float lfo_phase = 0.0f;

    void ensure_buffers(AudioArena* arena) {
        if (!arena) return;
        if (!buffer[0]) buffer[0] = arena->allocate(MAX_CHORUS_SAMPLES);
        if (!buffer[1]) buffer[1] = arena->allocate(MAX_CHORUS_SAMPLES);
    }

    void reset() {
        for (std::size_t ch = 0; ch < 2; ++ch) {
            if (buffer[ch]) {
                std::memset(buffer[ch], 0, MAX_CHORUS_SAMPLES * sizeof(float));
                write_pos[ch] = 0;
            }
        }
        lfo_phase = 0.0f;
    }
};

// Phaser state (per-channel cascaded allpass filters, stereo-native).
struct PhaserState {
    static constexpr std::size_t NUM_STAGES = 12;  // Max stages

    float allpass_state[2][NUM_STAGES] = {};  // [ch][stage]
    float allpass_delay[2][NUM_STAGES] = {};
    float lfo_phase = 0.0f;
    float last_output[2] = {0.0f, 0.0f};
};

// ============================================================================
// Sampler States
// ============================================================================

// Voice for polyphonic sample playback
struct SamplerVoice {
    float position = 0.0f;      // Current playback position in samples
    float speed = 1.0f;         // Playback speed (1.0 = original pitch)
    float velocity = 1.0f;      // Per-voice velocity baked at trigger time
    std::uint32_t sample_id = 0; // Which sample is playing
    bool active = false;        // Whether this voice is currently playing

    // Anti-click envelope (micro-fade)
    std::uint8_t attack_counter = 0;   // Counts up during attack (~5 samples)
    bool fading_out = false;           // Voice is being faded out (stolen)
    std::uint8_t fadeout_counter = 0;  // Counts up during fadeout
};

// Sampler state - polyphonic sample playback
struct SamplerState {
    static constexpr std::size_t MAX_VOICES = 32;  // Maximum simultaneous voices

    SamplerVoice voices[MAX_VOICES] = {};

    // Trigger detection
    float prev_trigger = 0.0f;

    // Allocate a new voice for playback (returns nullptr if all voices busy)
    SamplerVoice* allocate_voice() {
        // Find first inactive voice
        for (std::size_t i = 0; i < MAX_VOICES; ++i) {
            if (!voices[i].active) {
                return &voices[i];
            }
        }
        // No voice stealing - return nullptr if all busy
        return nullptr;
    }
};

// ============================================================================
// SoundFont Voice State
// ============================================================================

#ifndef CEDAR_NO_SOUNDFONT
// Per-voice state for SoundFont playback
struct SFVoice {
    float position = 0.0f;       // Current playback position in sample frames
    float speed = 1.0f;          // Playback speed (sample_rate ratio * pitch shift)
    std::uint32_t sample_id = 0; // Sample ID in SampleBank
    std::uint8_t note = 0;       // MIDI note number
    bool active = false;         // Voice currently producing audio
    bool releasing = false;      // Gate off, in release phase

    // Loop info (copied from zone at trigger time)
    std::uint32_t loop_start = 0;
    std::uint32_t loop_end = 0;
    std::uint32_t sample_end = 0;
    std::uint8_t loop_mode = 0;  // 0=none, 1=continuous, 3=sustain

    // Envelope state (DAHDSR)
    enum class EnvStage : std::uint8_t {
        Idle = 0, Delay, Attack, Hold, Decay, Sustain, Release
    };
    EnvStage env_stage = EnvStage::Idle;
    float env_level = 0.0f;      // Current envelope amplitude (0-1)
    float env_time = 0.0f;       // Time within current stage (seconds)
    // DAHDSR params (copied from zone at trigger time)
    float env_delay = 0.0f;
    float env_attack = 0.001f;
    float env_hold = 0.0f;
    float env_decay = 0.001f;
    float env_sustain = 1.0f;
    float env_release = 0.001f;

    // Level
    float attenuation_linear = 1.0f;  // Pre-computed from dB
    float pan = 0.0f;                 // -0.5 to +0.5
    float velocity_gain = 1.0f;       // Velocity-based amplitude scaling

    // Per-voice SVF lowpass filter state
    float filter_fc = 20000.0f;       // Cutoff frequency (Hz, from zone)
    float filter_q = 0.0f;            // Q in dB (from zone, 0-96)
    float filter_z1 = 0.0f;           // SVF state (ic1eq)
    float filter_z2 = 0.0f;           // SVF state (ic2eq)
    // Cached SVF coefficients (computed at note-on)
    float filter_a1 = 0.0f;
    float filter_a2 = 0.0f;
    float filter_a3 = 0.0f;
    bool filter_active = false;       // True if filter_fc < 19000

    // Micro-fade for voice stealing / re-trigger
    std::uint8_t fade_counter = 0;
    static constexpr std::uint8_t FADE_SAMPLES = 64;
};

// SoundFont voice state — uses arena-allocated voice array
// Keeps variant small (just pointer + metadata)
struct SoundFontVoiceState {
    static constexpr std::uint16_t MAX_VOICES = 32;

    SFVoice* voices = nullptr;    // Arena-allocated array
    std::uint16_t num_voices = 0; // Active voice count tracking
    float prev_gate = 0.0f;       // For gate edge detection
    std::uint8_t prev_note = 255; // For note-change fallback (255 = none)

    // Ensure voices are allocated from arena
    void ensure_voices(AudioArena* arena) {
        if (voices) return;
        if (!arena) return;
        // Allocate as raw float buffer, reinterpret as SFVoice array
        // sizeof(SFVoice) * MAX_VOICES / sizeof(float) gives us the float count
        constexpr std::size_t voice_floats = (sizeof(SFVoice) * MAX_VOICES + sizeof(float) - 1) / sizeof(float);
        float* raw = arena->allocate(voice_floats);
        if (raw) {
            voices = reinterpret_cast<SFVoice*>(raw);
            // Value-initialize each voice
            for (std::uint16_t i = 0; i < MAX_VOICES; ++i) {
                new (&voices[i]) SFVoice{};
            }
        }
    }

    // Find a free voice, or steal the quietest releasing voice
    SFVoice* allocate_voice(std::uint8_t /*note*/) {
        if (!voices) return nullptr;

        // First: find free voice
        for (std::uint16_t i = 0; i < MAX_VOICES; ++i) {
            if (!voices[i].active) {
                return &voices[i];
            }
        }

        // Second: steal quietest releasing voice
        SFVoice* quietest = nullptr;
        float min_level = 2.0f;
        for (std::uint16_t i = 0; i < MAX_VOICES; ++i) {
            if (voices[i].releasing && voices[i].env_level < min_level) {
                min_level = voices[i].env_level;
                quietest = &voices[i];
            }
        }
        if (quietest) {
            // Quick fade to avoid click
            quietest->fade_counter = 0;
            return quietest;
        }

        // Third: steal oldest active voice (highest position)
        SFVoice* oldest = nullptr;
        float max_pos = -1.0f;
        for (std::uint16_t i = 0; i < MAX_VOICES; ++i) {
            if (voices[i].position > max_pos) {
                max_pos = voices[i].position;
                oldest = &voices[i];
            }
        }
        if (oldest) {
            oldest->fade_counter = 0;
        }
        return oldest;
    }

    // Release all voices playing a specific note
    void release_note(std::uint8_t note) {
        if (!voices) return;
        for (std::uint16_t i = 0; i < MAX_VOICES; ++i) {
            if (voices[i].active && voices[i].note == note && !voices[i].releasing) {
                voices[i].releasing = true;
                voices[i].env_stage = SFVoice::EnvStage::Release;
                voices[i].env_time = 0.0f;
            }
        }
    }
};
#endif // CEDAR_NO_SOUNDFONT

// ============================================================================
// Dynamics States
// ============================================================================

// Compressor state
struct CompressorState {
    float envelope = 0.0f;
    float gain_reduction = 1.0f;

    // Cached coefficients
    float attack_coeff = 0.0f;
    float release_coeff = 0.0f;
    float last_attack = -1.0f;
    float last_release = -1.0f;
};

// Limiter state with lookahead
struct LimiterState {
    static constexpr std::size_t LOOKAHEAD_SAMPLES = 48;  // 1ms at 48kHz

    float lookahead_buffer[LOOKAHEAD_SAMPLES] = {};
    std::size_t write_pos = 0;
    float gain = 1.0f;
};

// Gate state
struct GateState {
    float envelope = 0.0f;
    float gain = 1.0f;   // Start at unity (gate open by default)
    bool is_open = true; // Match initial gain state
    float hold_counter = 0.0f;

    // Cached coefficients
    float attack_coeff = 0.0f;
    float release_coeff = 0.0f;
    float last_attack = -1.0f;
    float last_release = -1.0f;
};

// ============================================================================
// Reverb States
// ============================================================================

// Freeverb state (Schroeder-Moorer algorithm) with arena-allocated buffers.
//
// Per-channel duplicated for prd-stereo-native-opcodes Phase 2: index [0] = L
// lane, [1] = R lane. Each lane has its own full 8-comb + 4-allpass network.
// L/R decorrelation comes from the classic Schroeder +23-sample offset on the
// R lane buffer sizes — Freeverb is a parallel network (not figure-8), so the
// lanes do not cross-couple; the buffer-length offset is the entire source of
// width.
struct FreeverbState {
    static constexpr std::size_t NUM_COMBS = 8;
    static constexpr std::size_t NUM_ALLPASSES = 4;

    // Classic Schroeder stereo-spread offset (samples) added to the R lane
    // delay-line sizes. This is the canonical Freeverb stereo trick.
    static constexpr std::size_t STEREO_SPREAD = 23;

    // Comb filter delay times (samples at 48kHz, prime-like spacing).
    // L lane uses COMB_SIZES_LR[0], R lane uses COMB_SIZES_LR[1] (+spread).
    static constexpr std::size_t COMB_SIZES_LR[2][NUM_COMBS] = {
        {1557, 1617, 1491, 1422, 1277, 1356, 1188, 1116},
        {1557 + STEREO_SPREAD, 1617 + STEREO_SPREAD, 1491 + STEREO_SPREAD,
         1422 + STEREO_SPREAD, 1277 + STEREO_SPREAD, 1356 + STEREO_SPREAD,
         1188 + STEREO_SPREAD, 1116 + STEREO_SPREAD}
    };
    // Allpass delay times (same stereo-spread treatment).
    static constexpr std::size_t ALLPASS_SIZES_LR[2][NUM_ALLPASSES] = {
        {225, 556, 441, 341},
        {225 + STEREO_SPREAD, 556 + STEREO_SPREAD, 441 + STEREO_SPREAD,
         341 + STEREO_SPREAD}
    };

    // Arena-allocated buffers: [channel][filter_index]
    float* comb_buffers[2][NUM_COMBS] = {};
    std::size_t comb_pos[2][NUM_COMBS] = {};
    float comb_filter_state[2][NUM_COMBS] = {};

    // DC blocker state per comb (in feedback path)
    float dc_x1[2][NUM_COMBS] = {};
    float dc_y1[2][NUM_COMBS] = {};

    float* allpass_buffers[2][NUM_ALLPASSES] = {};
    std::size_t allpass_pos[2][NUM_ALLPASSES] = {};

    void ensure_buffers(AudioArena* arena) {
        if (!arena) return;
        for (std::size_t c = 0; c < 2; ++c) {
            for (std::size_t i = 0; i < NUM_COMBS; ++i) {
                if (!comb_buffers[c][i]) {
                    comb_buffers[c][i] = arena->allocate(COMB_SIZES_LR[c][i]);
                }
            }
            for (std::size_t i = 0; i < NUM_ALLPASSES; ++i) {
                if (!allpass_buffers[c][i]) {
                    allpass_buffers[c][i] = arena->allocate(ALLPASS_SIZES_LR[c][i]);
                }
            }
        }
    }

    void reset() {
        for (std::size_t c = 0; c < 2; ++c) {
            for (std::size_t i = 0; i < NUM_COMBS; ++i) {
                if (comb_buffers[c][i]) {
                    std::memset(comb_buffers[c][i], 0,
                                COMB_SIZES_LR[c][i] * sizeof(float));
                    comb_pos[c][i] = 0;
                    comb_filter_state[c][i] = 0.0f;
                    dc_x1[c][i] = 0.0f;
                    dc_y1[c][i] = 0.0f;
                }
            }
            for (std::size_t i = 0; i < NUM_ALLPASSES; ++i) {
                if (allpass_buffers[c][i]) {
                    std::memset(allpass_buffers[c][i], 0,
                                ALLPASS_SIZES_LR[c][i] * sizeof(float));
                    allpass_pos[c][i] = 0;
                }
            }
        }
    }
};

// Dattorro plate reverb state with arena-allocated buffers
struct DattorroState {
    static constexpr std::size_t NUM_INPUT_DIFFUSERS = 4;
    static constexpr std::size_t PREDELAY_SIZE = 4800;  // 100ms at 48kHz
    static constexpr std::size_t MAX_DELAY_SIZE = 5000;

    // Input diffuser sizes (samples)
    static constexpr std::size_t INPUT_DIFFUSER_SIZES[NUM_INPUT_DIFFUSERS] = {142, 107, 379, 277};
    // Decay diffuser sizes
    static constexpr std::size_t DECAY_DIFFUSER_SIZES[2] = {672, 908};
    // Tank delay sizes
    static constexpr std::size_t DELAY_SIZES[2] = {4453, 4217};

    // All buffers arena-allocated to keep variant size small.
    //
    // Predelay + input-diffuser network are duplicated per channel
    // (prd-stereo-native-opcodes Phase 0): index [0] = L lane, [1] = R lane.
    // On mono input the lanes receive identical samples and stay
    // synchronized; on stereo input they diverge naturally.
    float* predelay_buffer[2] = {};
    std::size_t predelay_pos[2] = {};

    float* input_diffusers[2][NUM_INPUT_DIFFUSERS] = {};
    std::size_t input_pos[2][NUM_INPUT_DIFFUSERS] = {};

    float* decay_diffusers[2] = {};
    std::size_t decay_pos[2] = {};

    float* delays[2] = {};
    std::size_t delay_pos[2] = {};

    // Damping filters
    float damp_state[2] = {};

    // DC blocker state per branch (in feedback path)
    float dc_x1[2] = {};
    float dc_y1[2] = {};

    // Tank feedback (for figure-8 topology)
    float tank_feedback[2] = {};

    // Modulation. Shared across L/R lanes — L/R decorrelation comes from the
    // existing 180° tank offset between delay 0 and delay 1.
    float mod_phase = 0.0f;

    void ensure_buffers(AudioArena* arena) {
        if (!arena) return;
        for (std::size_t c = 0; c < 2; ++c) {
            if (!predelay_buffer[c]) {
                predelay_buffer[c] = arena->allocate(PREDELAY_SIZE);
            }
            for (std::size_t i = 0; i < NUM_INPUT_DIFFUSERS; ++i) {
                if (!input_diffusers[c][i]) {
                    input_diffusers[c][i] = arena->allocate(INPUT_DIFFUSER_SIZES[i]);
                }
            }
        }
        for (std::size_t i = 0; i < 2; ++i) {
            if (!decay_diffusers[i]) {
                decay_diffusers[i] = arena->allocate(DECAY_DIFFUSER_SIZES[i]);
            }
            if (!delays[i]) {
                delays[i] = arena->allocate(MAX_DELAY_SIZE);
            }
        }
    }

    void reset() {
        for (std::size_t c = 0; c < 2; ++c) {
            if (predelay_buffer[c]) {
                std::memset(predelay_buffer[c], 0, PREDELAY_SIZE * sizeof(float));
            }
            predelay_pos[c] = 0;
            for (std::size_t i = 0; i < NUM_INPUT_DIFFUSERS; ++i) {
                if (input_diffusers[c][i]) {
                    std::memset(input_diffusers[c][i], 0,
                                INPUT_DIFFUSER_SIZES[i] * sizeof(float));
                    input_pos[c][i] = 0;
                }
            }
        }
        for (std::size_t i = 0; i < 2; ++i) {
            if (decay_diffusers[i]) {
                std::memset(decay_diffusers[i], 0, DECAY_DIFFUSER_SIZES[i] * sizeof(float));
                decay_pos[i] = 0;
            }
            if (delays[i]) {
                std::memset(delays[i], 0, MAX_DELAY_SIZE * sizeof(float));
                delay_pos[i] = 0;
            }
            damp_state[i] = 0.0f;
            dc_x1[i] = 0.0f;
            dc_y1[i] = 0.0f;
            tank_feedback[i] = 0.0f;
        }
        mod_phase = 0.0f;
    }
};

// FDN (Feedback Delay Network) state
struct FDNState {
    static constexpr std::size_t NUM_DELAYS = 4;
    static constexpr std::size_t MAX_DELAY_SIZE = 5000;

    // Prime-ratio delay sizes for dense reverb
    static constexpr std::size_t DELAY_SIZES[NUM_DELAYS] = {1931, 2473, 3181, 3671};

    // Arena-allocated buffers (not inline to keep variant size small)
    float* delay_buffers[NUM_DELAYS] = {};
    std::size_t write_pos[NUM_DELAYS] = {};
    float damp_state[NUM_DELAYS] = {};

    // DC blocker state per delay line (in feedback path)
    float dc_x1[NUM_DELAYS] = {};
    float dc_y1[NUM_DELAYS] = {};

    void ensure_buffers(AudioArena* arena) {
        if (!arena) return;
        for (std::size_t i = 0; i < NUM_DELAYS; ++i) {
            if (!delay_buffers[i]) {
                delay_buffers[i] = arena->allocate(MAX_DELAY_SIZE);
            }
        }
    }

    void reset() {
        for (std::size_t i = 0; i < NUM_DELAYS; ++i) {
            if (delay_buffers[i]) {
                std::fill_n(delay_buffers[i], MAX_DELAY_SIZE, 0.0f);
            }
            write_pos[i] = 0;
            damp_state[i] = 0.0f;
            dc_x1[i] = 0.0f;
            dc_y1[i] = 0.0f;
        }
    }
};

// ============================================================================
// Stereo Effect States
// ============================================================================

// Ping-pong delay state with arena-allocated stereo buffers
struct PingPongDelayState {
    // Maximum delay time: 10 seconds at 96kHz = 960000 samples
    static constexpr std::size_t MAX_DELAY_SAMPLES = 960000;

    // Separate stereo delay lines
    float* buffer_left = nullptr;
    float* buffer_right = nullptr;
    std::size_t buffer_size = 0;
    std::size_t write_pos = 0;

    // Smoothed delay time (prevents clicks)
    float smoothed_delay = 0.0f;
    bool initialized = false;

    // Damping filter state for each channel
    float damp_state_left = 0.0f;
    float damp_state_right = 0.0f;

    void ensure_buffers(std::size_t samples, AudioArena* arena) {
        if (buffer_left && buffer_size >= samples) {
            return;  // Already have enough space
        }
        if (!arena) return;

        std::size_t new_size = std::min(samples, MAX_DELAY_SAMPLES);
        float* new_buffer_left = arena->allocate(new_size);
        float* new_buffer_right = arena->allocate(new_size);
        if (new_buffer_left && new_buffer_right) {
            buffer_left = new_buffer_left;
            buffer_right = new_buffer_right;
            buffer_size = new_size;
            write_pos = 0;
        }
    }

    void reset() {
        if (buffer_left && buffer_size > 0) {
            std::memset(buffer_left, 0, buffer_size * sizeof(float));
        }
        if (buffer_right && buffer_size > 0) {
            std::memset(buffer_right, 0, buffer_size * sizeof(float));
        }
        write_pos = 0;
        smoothed_delay = 0.0f;
        initialized = false;
        damp_state_left = 0.0f;
        damp_state_right = 0.0f;
    }
};

// ============================================================================
// Polyphony State
// ============================================================================

// NoteEvent bridges pattern events and MIDI to voice allocation
struct NoteEvent {
    float freq;                    // Hz
    float vel;                     // 0-1
    std::uint16_t event_index;     // Source OutputEvent index
    std::uint16_t voice_index;     // Which chord note (0-3)
    std::uint32_t gate_on_sample;  // Sample offset within block for note-on
    std::uint32_t gate_off_sample; // Sample offset for note-off (BLOCK_SIZE if sustaining)
};

// Voice slot for POLY block
struct PolyVoice {
    float freq = 0.0f;
    float vel = 0.0f;
    float gate = 0.0f;
    bool active = false;
    bool releasing = false;
    std::uint32_t age = 0;
    std::int8_t note = -1;
    std::uint16_t event_index = 0xFFFF;
    std::uint32_t cycle = 0;
    std::uint32_t pending_gate_on = BLOCK_SIZE;   // BLOCK_SIZE = no pending on
    std::uint32_t pending_gate_off = BLOCK_SIZE;  // BLOCK_SIZE = no pending off
};

// PolyAllocState — arena-allocated voices (same pattern as SoundFontVoiceState)
struct PolyAllocState {
    static constexpr std::uint16_t MAX_VOICES = 128;

    PolyVoice* voices = nullptr;          // Arena-allocated array
    std::uint32_t seq_state_id = 0;       // Linked SequenceState for events
    std::uint8_t max_voices = 8;
    std::uint8_t mode = 0;               // 0=poly, 1=mono, 2=legato
    std::uint8_t steal_strategy = 0;     // 0=oldest, 1=quietest

    void ensure_voices(AudioArena* arena) {
        if (voices) return;
        if (!arena) return;
        constexpr std::size_t voice_floats =
            (sizeof(PolyVoice) * MAX_VOICES + sizeof(float) - 1) / sizeof(float);
        float* raw = arena->allocate(voice_floats);
        if (raw) {
            voices = reinterpret_cast<PolyVoice*>(raw);
            for (std::uint16_t i = 0; i < MAX_VOICES; ++i) {
                new (&voices[i]) PolyVoice{};
            }
        }
    }

    std::uint8_t active_voice_count() const {
        if (!voices) return 0;
        std::uint8_t count = 0;
        for (std::uint16_t i = 0; i < max_voices; ++i) {
            if (voices[i].active) ++count;
        }
        return count;
    }

    // Allocate a voice for a new note, returns voice index or -1 if failed
    int allocate_voice(float freq, float vel, std::uint16_t event_idx,
                       std::uint32_t cyc, std::uint32_t gate_on_sample) {
        if (!voices) return -1;

        if (mode == 1 || mode == 2) {
            // Mono/Legato: always use voice 0
            auto& v = voices[0];
            bool was_active = v.active;
            v.freq = freq;
            v.vel = vel;
            v.active = true;
            v.releasing = false;
            v.event_index = event_idx;
            v.cycle = cyc;
            v.pending_gate_on = gate_on_sample;
            v.pending_gate_off = BLOCK_SIZE;
            // Mono: always retrigger. Legato: only trigger if wasn't active
            if (mode == 1 || !was_active) {
                v.gate = 1.0f;
                v.age = 0;
            }
            // Legato with already-active voice: update freq/vel but keep gate
            return 0;
        }

        // Poly mode: reuse active voice with same frequency
        // Preserves oscillator phase (same voice slot = same XOR isolation)
        // but retriggers gate so envelopes re-attack for clear chord articulation
        for (std::uint16_t i = 0; i < max_voices; ++i) {
            if (voices[i].active && std::abs(voices[i].freq - freq) < 0.5f) {
                auto& v = voices[i];
                v.vel = vel;
                v.event_index = event_idx;
                v.cycle = cyc;
                v.releasing = false;
                v.gate = 1.0f;
                v.age = 0;
                v.pending_gate_on = gate_on_sample;  // Retrigger for envelope
                v.pending_gate_off = BLOCK_SIZE;
                return static_cast<int>(i);
            }
        }

        // Find first inactive slot
        for (std::uint16_t i = 0; i < max_voices; ++i) {
            if (!voices[i].active) {
                auto& v = voices[i];
                v.freq = freq;
                v.vel = vel;
                v.gate = 1.0f;
                v.active = true;
                v.releasing = false;
                v.age = 0;
                v.event_index = event_idx;
                v.cycle = cyc;
                v.pending_gate_on = gate_on_sample;
                v.pending_gate_off = BLOCK_SIZE;
                return static_cast<int>(i);
            }
        }

        // All voices active — steal oldest
        // Prefer stealing a releasing voice first
        int steal_idx = -1;
        std::uint32_t max_age = 0;
        for (std::uint16_t i = 0; i < max_voices; ++i) {
            if (voices[i].releasing && voices[i].age >= max_age) {
                max_age = voices[i].age;
                steal_idx = static_cast<int>(i);
            }
        }
        if (steal_idx < 0) {
            // No releasing voices — steal oldest active
            max_age = 0;
            for (std::uint16_t i = 0; i < max_voices; ++i) {
                if (voices[i].age >= max_age) {
                    max_age = voices[i].age;
                    steal_idx = static_cast<int>(i);
                }
            }
        }
        if (steal_idx >= 0) {
            auto& v = voices[steal_idx];
            v.freq = freq;
            v.vel = vel;
            v.gate = 1.0f;
            v.active = true;
            v.releasing = false;
            v.age = 0;
            v.event_index = event_idx;
            v.cycle = cyc;
            v.pending_gate_on = gate_on_sample;
            v.pending_gate_off = BLOCK_SIZE;
        }
        return steal_idx;
    }

    // Release voices whose event_index+cycle match
    void release_voice_by_event(std::uint16_t event_idx, std::uint32_t cyc,
                                std::uint32_t gate_off_sample) {
        if (!voices) return;

        if (mode == 1 || mode == 2) {
            // Mono/legato: only release if current voice matches
            auto& v = voices[0];
            if (v.active && v.event_index == event_idx && v.cycle == cyc) {
                v.releasing = true;
                v.gate = 0.0f;
                v.age = 0;
                v.pending_gate_off = gate_off_sample;
            }
            return;
        }

        // Poly mode: find and release matching voices
        for (std::uint16_t i = 0; i < max_voices; ++i) {
            auto& v = voices[i];
            if (v.active && !v.releasing &&
                v.event_index == event_idx && v.cycle == cyc) {
                v.releasing = true;
                v.gate = 0.0f;
                v.age = 0;
                v.pending_gate_off = gate_off_sample;
            }
        }
    }

    // Release timeout: blocks after gate-off before voice is deactivated
    // (gate multiplication zeros output during this window)
    static constexpr std::uint32_t RELEASE_TIMEOUT = 4;

    void tick() {
        if (!voices) return;
        for (std::uint16_t i = 0; i < max_voices; ++i) {
            if (voices[i].active) {
                voices[i].age++;
                // Clean up voices that have been releasing past timeout
                if (voices[i].releasing && voices[i].age > RELEASE_TIMEOUT) {
                    voices[i].active = false;
                    voices[i].releasing = false;
                }
            }
        }
    }
};

// ============================================================================
// Visualization States
// ============================================================================

// ============================================================================
// Extended Parameters
// ============================================================================

// Extended parameters for opcodes needing more than 5 inputs.
// Each slot can be either a compile-time constant or a runtime buffer reference.
// Stored in StatePool using the same state_id as the opcode.
template<std::size_t N>
struct ExtendedParams {
    struct Slot {
        float constant = 0.0f;              // Compile-time constant value
        std::uint16_t buffer_idx = 0xFFFF;  // Buffer index, or 0xFFFF = use constant

        [[nodiscard]] bool is_constant() const { return buffer_idx == 0xFFFF; }
    };

    std::array<Slot, N> params{};

    // Get parameter value at sample index
    // buffers: array of buffer pointers from ExecutionContext
    [[nodiscard]] float get(std::size_t param_idx, std::size_t sample_idx,
                           const float* const* buffers) const {
        const auto& slot = params[param_idx];
        if (slot.is_constant()) {
            return slot.constant;
        }
        return buffers[slot.buffer_idx][sample_idx];
    }

    // Set a parameter slot to a constant value
    void set_constant(std::size_t param_idx, float value) {
        params[param_idx].constant = value;
        params[param_idx].buffer_idx = 0xFFFF;
    }

    // Set a parameter slot to a buffer reference
    void set_buffer(std::size_t param_idx, std::uint16_t buffer_idx) {
        params[param_idx].buffer_idx = buffer_idx;
    }
};

// Composition helper: combines a DSP state with extended parameters
// Use when an opcode needs both state (e.g., filter memory) AND extra params
template<typename StateT, std::size_t N>
struct StateWithExtParams {
    StateT state{};
    ExtendedParams<N> ext{};
};

// ============================================================================
// Visualization States
// ============================================================================

// Probe state for signal capture (oscilloscope, waveform, spectrum)
// Uses a ring buffer to store recent samples for visualization
struct ProbeState {
    // Buffer size: ~21ms at 48kHz = 1024 samples
    // This is enough for oscilloscope and spectrum (1024-point FFT)
    static constexpr std::size_t PROBE_BUFFER_SIZE = 1024;

    float buffer[PROBE_BUFFER_SIZE] = {0.0f};
    std::size_t write_pos = 0;
    bool initialized = false;

    void reset() {
        std::memset(buffer, 0, sizeof(buffer));
        write_pos = 0;
        initialized = false;
    }

    // Write a block of samples to the ring buffer
    void write_block(const float* samples, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            buffer[write_pos] = samples[i];
            write_pos = (write_pos + 1) % PROBE_BUFFER_SIZE;
        }
        initialized = true;
    }

    // Get number of valid samples
    std::size_t sample_count() const {
        return initialized ? PROBE_BUFFER_SIZE : 0;
    }
};

#ifndef CEDAR_NO_FFT
// FFT probe state for spectral visualization (waterfall, spectrum)
// Accumulates samples and computes FFT when a full frame is collected.
// Buffers are arena-allocated to keep variant size small.
struct FFTProbeState {
    static constexpr std::size_t MAX_FFT_SIZE = 2048;
    static constexpr std::size_t MAX_BINS = MAX_FFT_SIZE / 2 + 1;  // 1025

    // Arena-allocated buffers (not inline to keep DSPState variant small)
    float* input_buffer = nullptr;    // [MAX_FFT_SIZE]
    float* magnitudes_db = nullptr;   // [MAX_BINS]
    float* real_bins = nullptr;       // [MAX_BINS]
    float* imag_bins = nullptr;       // [MAX_BINS]

    std::size_t write_pos = 0;
    std::size_t fft_size = 1024;

    // Frame counter - increments each completed FFT, used by viz to detect new data
    std::uint32_t frame_counter = 0;
    bool initialized = false;

    // Write a block of samples. Triggers FFT when accumulation buffer is full.
    // Implementation in cedar/src/dsp/fft.cpp
    void write_block(const float* samples, std::size_t count);
};
#endif // CEDAR_NO_FFT

// Variant holding all possible DSP state types
// std::monostate represents stateless operations
using DSPState = std::variant<
    std::monostate,
    OscState,
    SmoochState,
#ifndef CEDAR_NO_MINBLEP
    MinBLEPOscState,
#endif
    OscState4x,
    SVFState,
    NoiseState,
    SlewState,
    EdgeState,
    CellState,
    DelayState,
    EnvState,
    // Sequencing states
    LFOState,
    EuclidState,
    TriggerState,
    TransportState,
    TimelineState,
    SequenceState,      // Lazy queryable patterns (sequence system)
    // Filter states
    MoogState,
    DiodeState,
    FormantState,
    SallenkeyState,
    // Distortion states
    BitcrushState,
    SmoothSatState,
    TubeState,
    TapeState,
    XfmrState,
    ExciterState,
    FoldADAAState,
    // Modulation states
    CombFilterState,
    FlangerState,
    ChorusState,
    PhaserState,
    // Sampler states
    SamplerState,
#ifndef CEDAR_NO_SOUNDFONT
    SoundFontVoiceState,
#endif
    // Dynamics states
    CompressorState,
    LimiterState,
    GateState,
    // Reverb states
    FreeverbState,
    DattorroState,
    FDNState,
    // Stereo effect states
    PingPongDelayState,
    // Polyphony state
    PolyAllocState,
    // Visualization states
    ProbeState,
#ifndef CEDAR_NO_FFT
    FFTProbeState,
#endif
    // Extended parameters (for opcodes with 6+ params). The runtime
    // dispatch helper picks the smallest variant that fits ext_count, so
    // small migrations (e.g. one tunable param like chorus lfo_phase) don't
    // pay for an 8-slot allocation.
    ExtendedParams<1>,   // 6-param opcodes (5 inputs + 1 extended)
    ExtendedParams<2>,   // 7-param opcodes (5 inputs + 2 extended)
    ExtendedParams<3>,   // 8-param opcodes (5 inputs + 3 extended)
    ExtendedParams<5>,   // 10-param opcodes (5 inputs + 5 extended)
    ExtendedParams<8>    // 13-param opcodes (5 inputs + 8 extended)
>;

}  // namespace cedar
