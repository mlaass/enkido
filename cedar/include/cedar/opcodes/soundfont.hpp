#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../vm/sample_bank.hpp"
#include "../vm/state_pool.hpp"
#include "../audio/soundfont.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include "midi.hpp"  // for HELD_DURATION_SENTINEL
#include "sequence.hpp"  // for OutputEvents
#include <cmath>
#include <algorithm>

namespace cedar {

namespace sf_detail {

// Trigger SoundFont voices for one note-on. Shared between the legacy
// buffer-driven path (per-sample edge detection) and the Phase 7.1
// event-driven path (OutputEvents from MidiQueueState).
//
// `midi_note_f` is the fractional MIDI value (used for microtonal speed
// computation). `velocity_f` is the normalized 0..1 velocity (used for
// gain). `vel` is the 7-bit integer form (for zone velocity range
// matching). `note` is the 7-bit integer form (for zone key range matching).
inline void trigger_sf_voices_for_note(SoundFontVoiceState& state,
                                       const SoundFontBank& bank,
                                       const SoundFontPreset& preset,
                                       std::uint8_t note,
                                       std::uint8_t vel,
                                       float velocity_f,
                                       float midi_note_f,
                                       float sample_rate) {
    if (vel == 0) return;

    // Quick fade-out on same-note re-trigger.
    for (std::uint16_t v = 0; v < SoundFontVoiceState::MAX_VOICES; ++v) {
        if (state.voices[v].active && state.voices[v].note == note) {
            state.voices[v].releasing = true;
            state.voices[v].env_stage = SFVoice::EnvStage::Release;
            state.voices[v].env_time = 0.0f;
            state.voices[v].env_release = 0.005f;
            state.voices[v].env_release_start_level = state.voices[v].env_level;
        }
    }

    const SoundFontZone* zones[8];
    std::size_t zone_count = bank.find_zones(preset, note, vel, zones, 8);

    for (std::size_t z = 0; z < zone_count; ++z) {
        const SoundFontZone& zone = *zones[z];
        if (zone.sample_id == 0) continue;

        SFVoice* voice = state.allocate_voice(note);
        if (!voice) break;

        voice->active = true;
        voice->releasing = false;
        voice->note = note;
        voice->position = 0.0f;
        voice->fade_counter = SFVoice::FADE_SAMPLES;

        float pitch_cents = (midi_note_f - static_cast<float>(zone.root_key)
                             + static_cast<float>(zone.transpose)) * 100.0f
                            + static_cast<float>(zone.tune);
        float pitch_ratio = std::pow(2.0f, pitch_cents / 1200.0f);
        voice->speed = pitch_ratio * (zone.sample_rate / sample_rate);

        voice->sample_id = zone.sample_id;
        voice->loop_start = zone.loop_start;
        voice->loop_end = zone.loop_end;
        voice->sample_end = zone.sample_end;
        voice->loop_mode = static_cast<std::uint8_t>(zone.loop_mode);

        voice->attenuation_linear = std::pow(10.0f, -zone.attenuation / 20.0f);
        voice->pan = zone.pan;
        voice->velocity_gain = velocity_f;

        voice->env_delay = zone.amp_env.delay;
        voice->env_attack = std::max(0.001f, zone.amp_env.attack);
        voice->env_hold = zone.amp_env.hold;
        voice->env_decay = std::max(0.001f, zone.amp_env.decay);
        voice->env_sustain = zone.amp_env.sustain;
        voice->env_release = std::max(0.001f, zone.amp_env.release);

        voice->env_stage = (voice->env_delay > 0.0f)
            ? SFVoice::EnvStage::Delay
            : SFVoice::EnvStage::Attack;
        voice->env_level = 0.0f;
        voice->env_time = 0.0f;

        float fc = zone.filter_fc;
        if (zone.mod_env_to_filter_fc != 0) {
            float key_offset = midi_note_f - 60.0f;
            fc *= std::pow(2.0f, key_offset *
                static_cast<float>(zone.mod_env_to_filter_fc) / (1200.0f * 12.0f));
        }
        fc = std::clamp(fc, 20.0f, sample_rate * 0.49f);
        voice->filter_fc = fc;
        voice->filter_q = zone.filter_q;
        voice->filter_z1 = 0.0f;
        voice->filter_z2 = 0.0f;
        voice->filter_active = (fc < 19000.0f);

        if (voice->filter_active) {
            float q_db = std::max(1.0f, zone.filter_q);
            float q_linear = std::max(0.5f, std::pow(10.0f, q_db / 20.0f));
            float g = std::tan(PI * fc / sample_rate);
            float k = 1.0f / q_linear;
            voice->filter_a1 = 1.0f / (1.0f + g * (g + k));
            voice->filter_a2 = g * voice->filter_a1;
            voice->filter_a3 = g * voice->filter_a2;
        }
    }
}

// Per-sample voice mix. Advances envelope, reads sample, applies filter,
// accumulates into `output_accum`. Used by both the legacy and the
// event-driven path. Returns nothing — output is in/out via reference.
inline void process_sf_voice_one_sample(SFVoice& voice,
                                        SampleBank* sample_bank,
                                        float inv_sr,
                                        float& output_accum) {
    if (!voice.active) return;

    float env = voice.env_level;
    switch (voice.env_stage) {
        case SFVoice::EnvStage::Idle:
            env = 0.0f;
            break;
        case SFVoice::EnvStage::Delay:
            env = 0.0f;
            voice.env_time += inv_sr;
            if (voice.env_time >= voice.env_delay) {
                voice.env_stage = SFVoice::EnvStage::Attack;
                voice.env_time = 0.0f;
            }
            break;
        case SFVoice::EnvStage::Attack:
            voice.env_time += inv_sr;
            env = voice.env_time / voice.env_attack;
            if (env >= 1.0f) {
                env = 1.0f;
                voice.env_stage = SFVoice::EnvStage::Hold;
                voice.env_time = 0.0f;
            }
            break;
        case SFVoice::EnvStage::Hold:
            env = 1.0f;
            voice.env_time += inv_sr;
            if (voice.env_time >= voice.env_hold) {
                voice.env_stage = SFVoice::EnvStage::Decay;
                voice.env_time = 0.0f;
            }
            break;
        case SFVoice::EnvStage::Decay: {
            voice.env_time += inv_sr;
            float decay_progress = voice.env_time / voice.env_decay;
            if (decay_progress >= 1.0f) {
                env = voice.env_sustain;
                voice.env_stage = SFVoice::EnvStage::Sustain;
            } else {
                float t = 1.0f - std::exp(-5.0f * decay_progress);
                env = 1.0f + (voice.env_sustain - 1.0f) * t;
            }
            break;
        }
        case SFVoice::EnvStage::Sustain:
            env = voice.env_sustain;
            break;
        case SFVoice::EnvStage::Release: {
            voice.env_time += inv_sr;
            float release_progress = voice.env_time / voice.env_release;
            if (release_progress >= 1.0f) {
                env = 0.0f;
                voice.active = false;
            } else {
                // Decay from the level captured at gate-off (env_release_start_level)
                // via a single exponential. Reading voice.env_level here and
                // writing back at line 184 would compound the decay each
                // sample and collapse a 100ms release into ~10ms (audible click).
                env = voice.env_release_start_level * std::exp(-5.0f * release_progress);
            }
            break;
        }
    }
    voice.env_level = env;

    if (env < 1e-6f && voice.env_stage == SFVoice::EnvStage::Release) {
        voice.active = false;
        return;
    }

    const SampleData* sample = sample_bank->get_sample(voice.sample_id);
    if (!sample || sample->frames == 0) {
        voice.active = false;
        return;
    }

    float sample_value = 0.0f;
    float pos = voice.position;
    if (pos >= 0.0f && pos < static_cast<float>(voice.sample_end)) {
        for (std::uint32_t ch = 0; ch < sample->channels; ++ch) {
            sample_value += sample->get_interpolated(pos, ch);
        }
        sample_value /= static_cast<float>(sample->channels);
    }

    if (voice.filter_active) {
        float v3 = sample_value - voice.filter_z2;
        float v1 = voice.filter_a1 * voice.filter_z1 + voice.filter_a2 * v3;
        float v2 = voice.filter_z2 + voice.filter_a2 * voice.filter_z1 +
                   voice.filter_a3 * v3;
        voice.filter_z1 = 2.0f * v1 - voice.filter_z1;
        voice.filter_z2 = 2.0f * v2 - voice.filter_z2;
        sample_value = v2;
    }

    float gain = env * voice.attenuation_linear * voice.velocity_gain;
    if (voice.fade_counter < SFVoice::FADE_SAMPLES) {
        gain *= static_cast<float>(voice.fade_counter) /
                static_cast<float>(SFVoice::FADE_SAMPLES);
        voice.fade_counter++;
    }

    output_accum += sample_value * gain;
    voice.position += voice.speed;

    if (voice.loop_mode == 1 || (voice.loop_mode == 3 && !voice.releasing)) {
        if (voice.loop_end > voice.loop_start) {
            float loop_len = static_cast<float>(voice.loop_end - voice.loop_start);
            while (voice.position >= static_cast<float>(voice.loop_end)) {
                voice.position -= loop_len;
            }
        }
    }

    if (voice.position >= static_cast<float>(voice.sample_end)) {
        voice.active = false;
    }
}

}  // namespace sf_detail

// ============================================================================
// SOUNDFONT_VOICE: Polyphonic SoundFont playback
// ============================================================================
// in0: gate signal (>0 = note on, 0→positive edge = trigger, positive→0 = release)
// in1: frequency in Hz (converted to MIDI note internally)
// in2: velocity (0-1)
// in3: preset index (constant, which preset to use)
// in4: trigger pulse (optional, from SEQPAT_STEP — 0xFFFF if not wired)
// rate: soundfont ID (0-255)
//
// Multi-zone, polyphonic SoundFont sampler.
// - Frequency-to-MIDI conversion for zone lookup
// - Pitch shifting based on root key and tuning
// - DAHDSR envelope per voice
// - SF2 loop modes (none, continuous, sustain)
// - Voice stealing when all 32 voices are active
[[gnu::always_inline]]
inline void op_soundfont_voice(ExecutionContext& ctx, const Instruction& inst,
                                SampleBank* sample_bank, SoundFontRegistry* sf_registry) {
    float* out = ctx.buffers->get(inst.out_buffer);

    auto& state = ctx.states->get_or_create<SoundFontVoiceState>(inst.state_id);
    state.ensure_voices(ctx.arena);

    if (!state.voices || !sf_registry || !sample_bank) {
        // No voices allocated or no SF2 loaded — output silence
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) out[i] = 0.0f;
        return;
    }

    // Get SoundFont (constant across block)
    int sf_id = static_cast<int>(inst.rate);
    const SoundFontBank* bank = sf_registry->get(sf_id);
    if (!bank) {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) out[i] = 0.0f;
        return;
    }

    const float inv_sr = 1.0f / ctx.sample_rate;

    // PRD prd-midi-input §7.1: event-driven path. When inst.inputs[0] is
    // unwired the codegen has selected the MIDI-upstream form — drain
    // OutputEvents directly into allocate_voice / release_note instead of
    // the per-sample edge-detection on gate/freq/vel buffers. seq_state_id
    // and preset_idx live on the state (set by init_soundfont_voice_event_state).
    // Voice mixing reuses process_sf_voice_one_sample so output shape
    // matches the legacy path exactly.
    if (inst.inputs[0] == 0xFFFFu) {
        const SoundFontPreset* preset = bank->get_preset_by_index(
            static_cast<std::size_t>(std::max(0, state.preset_idx)));
        if (!preset) {
            for (std::size_t i = 0; i < BLOCK_SIZE; ++i) out[i] = 0.0f;
            return;
        }

        auto events_src = ctx.states->resolve_output_events(state.seq_state_id);
        if (events_src.events && events_src.events->num_events > 0) {
            const double spb_d = (60.0 / static_cast<double>(ctx.bpm))
                               * static_cast<double>(ctx.sample_rate);
            if (spb_d > 0.0) {
                const double block_start_b =
                    static_cast<double>(ctx.global_sample_counter) / spb_d;
                const double block_end_b =
                    block_start_b + static_cast<double>(BLOCK_SIZE) / spb_d;
                constexpr float SENTINEL_THRESHOLD =
                    MidiQueueState::HELD_DURATION_SENTINEL * 0.5f;
                constexpr double BOUNDARY_EPS = 1e-9;

                const auto& output_events = *events_src.events;
                for (std::uint32_t e = 0; e < output_events.num_events; ++e) {
                    const auto& evt = output_events.events[e];

                    // Trigger on note-on transitions that fall in this block.
                    // Multi-value events (chords) trigger one voice per pitch.
                    const double on_b = static_cast<double>(evt.time);
                    if (on_b + BOUNDARY_EPS >= block_start_b &&
                        on_b < block_end_b) {
                        const std::uint8_t vel =
                            static_cast<std::uint8_t>(
                                std::clamp(evt.velocity, 0.0f, 1.0f) * 127.0f);
                        for (std::uint8_t vi = 0; vi < evt.num_values; ++vi) {
                            const float freq = evt.values[vi];
                            const float midi_note_f = (freq > 1.0f)
                                ? 69.0f + 12.0f * std::log2(freq / 440.0f)
                                : 0.0f;
                            const std::uint8_t note =
                                static_cast<std::uint8_t>(
                                    std::clamp(std::roundf(midi_note_f),
                                               0.0f, 127.0f));
                            sf_detail::trigger_sf_voices_for_note(
                                state, *bank, *preset, note, vel,
                                evt.velocity, midi_note_f, ctx.sample_rate);
                        }
                    }

                    // Release on note-off transitions that fall in this block.
                    if (evt.duration < SENTINEL_THRESHOLD) {
                        const double off_b =
                            on_b + static_cast<double>(evt.duration);
                        if (off_b + BOUNDARY_EPS >= block_start_b &&
                            off_b < block_end_b) {
                            for (std::uint8_t vi = 0; vi < evt.num_values; ++vi) {
                                const float freq = evt.values[vi];
                                const float midi_note_f = (freq > 1.0f)
                                    ? 69.0f + 12.0f * std::log2(freq / 440.0f)
                                    : 0.0f;
                                const std::uint8_t note =
                                    static_cast<std::uint8_t>(
                                        std::clamp(std::roundf(midi_note_f),
                                                   0.0f, 127.0f));
                                state.release_note(note);
                            }
                        }
                    }
                }
            }
        }

        // Per-sample mix of every active voice. Matches the legacy mix loop
        // body sans gate-edge detection.
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
            float output = 0.0f;
            for (std::uint16_t v = 0;
                 v < SoundFontVoiceState::MAX_VOICES; ++v) {
                sf_detail::process_sf_voice_one_sample(
                    state.voices[v], sample_bank, inv_sr, output);
            }
            out[i] = output;
        }
        return;
    }

    // Legacy buffer-driven path (pattern → soundfont). Unchanged.
    const float* gate = ctx.buffers->get(inst.inputs[0]);
    const float* freq_buf = ctx.buffers->get(inst.inputs[1]);
    const float* vel_buf = ctx.buffers->get(inst.inputs[2]);
    const float* preset_buf = ctx.buffers->get(inst.inputs[3]);
    const float* trig_buf = (inst.inputs[4] != 0xFFFF)
        ? ctx.buffers->get(inst.inputs[4]) : nullptr;

    int preset_idx = static_cast<int>(preset_buf[0]);
    const SoundFontPreset* preset = bank->get_preset_by_index(
        static_cast<std::size_t>(std::max(0, preset_idx)));
    if (!preset) {
        for (std::size_t i = 0; i < BLOCK_SIZE; ++i) out[i] = 0.0f;
        return;
    }

    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        float current_gate = gate[i];
        float freq = freq_buf[i];
        float velocity = std::clamp(vel_buf[i], 0.0f, 1.0f);

        // Convert frequency (Hz) to MIDI note number
        // midi = 69 + 12 * log2(freq / 440)
        float midi_note = (freq > 1.0f)
            ? 69.0f + 12.0f * std::log2(freq / 440.0f)
            : 0.0f;
        std::uint8_t note = static_cast<std::uint8_t>(std::clamp(std::roundf(midi_note), 0.0f, 127.0f));
        std::uint8_t vel = static_cast<std::uint8_t>(velocity * 127.0f);

        // Gate edge detection
        bool gate_on = (current_gate > 0.0f && state.prev_gate <= 0.0f);
        bool gate_off = (current_gate <= 0.0f && state.prev_gate > 0.0f);

        // Trigger pulse from pattern sequencer (fires at every event boundary)
        bool trigger_on = (trig_buf != nullptr && trig_buf[i] > 0.0f);

        // Fallback: note-change detection for non-pattern inputs (no trigger wired)
        bool note_change = (trig_buf == nullptr && current_gate > 0.0f
                            && note != state.prev_note && state.prev_note != 255);

        state.prev_gate = current_gate;
        state.prev_note = (current_gate > 0.0f) ? note : static_cast<std::uint8_t>(255);

        // Note on: find zones and allocate voices
        if ((gate_on || trigger_on || note_change) && vel > 0) {
            // Quick fade-out on same-note re-trigger
            for (std::uint16_t v = 0; v < SoundFontVoiceState::MAX_VOICES; ++v) {
                if (state.voices[v].active && state.voices[v].note == note) {
                    state.voices[v].releasing = true;
                    state.voices[v].env_stage = SFVoice::EnvStage::Release;
                    state.voices[v].env_time = 0.0f;
                    state.voices[v].env_release = 0.005f; // Fast re-trigger release
                    state.voices[v].env_release_start_level = state.voices[v].env_level;
                }
            }

            // Find matching zones
            const SoundFontZone* zones[8];
            std::size_t zone_count = bank->find_zones(*preset, note, vel, zones, 8);

            for (std::size_t z = 0; z < zone_count; ++z) {
                const SoundFontZone& zone = *zones[z];
                if (zone.sample_id == 0) continue;

                SFVoice* voice = state.allocate_voice(note);
                if (!voice) break;

                // Initialize voice
                voice->active = true;
                voice->releasing = false;
                voice->note = note;
                voice->position = 0.0f;
                voice->fade_counter = SFVoice::FADE_SAMPLES; // No fade needed for fresh voice

                // Compute playback speed using fractional midi_note for microtonal precision
                // Speed = 2^((midi - root_key + transpose) / 12 + tune/1200) * zone_sr/ctx_sr
                float pitch_cents = (midi_note - static_cast<float>(zone.root_key)
                                     + static_cast<float>(zone.transpose)) * 100.0f
                                    + static_cast<float>(zone.tune);
                float pitch_ratio = std::pow(2.0f, pitch_cents / 1200.0f);
                voice->speed = pitch_ratio * (zone.sample_rate / ctx.sample_rate);

                // Copy zone sample/loop info
                voice->sample_id = zone.sample_id;
                voice->loop_start = zone.loop_start;
                voice->loop_end = zone.loop_end;
                voice->sample_end = zone.sample_end;
                voice->loop_mode = static_cast<std::uint8_t>(zone.loop_mode);

                // Attenuation: convert dB to linear
                voice->attenuation_linear = std::pow(10.0f, -zone.attenuation / 20.0f);
                voice->pan = zone.pan;
                voice->velocity_gain = velocity;

                // Envelope parameters
                voice->env_delay = zone.amp_env.delay;
                voice->env_attack = std::max(0.001f, zone.amp_env.attack);
                voice->env_hold = zone.amp_env.hold;
                voice->env_decay = std::max(0.001f, zone.amp_env.decay);
                voice->env_sustain = zone.amp_env.sustain;
                voice->env_release = std::max(0.001f, zone.amp_env.release);

                // Start envelope
                if (voice->env_delay > 0.0f) {
                    voice->env_stage = SFVoice::EnvStage::Delay;
                } else {
                    voice->env_stage = SFVoice::EnvStage::Attack;
                }
                voice->env_level = 0.0f;
                voice->env_time = 0.0f;

                // Per-voice SVF lowpass filter
                // Key tracking: shift cutoff relative to middle C (note 60)
                float fc = zone.filter_fc;
                if (zone.mod_env_to_filter_fc != 0) {
                    // Key-to-filter: scale by semitone distance from middle C
                    // Use fractional midi_note for microtonal precision
                    float key_offset = midi_note - 60.0f;
                    fc *= std::pow(2.0f, key_offset * static_cast<float>(zone.mod_env_to_filter_fc) / (1200.0f * 12.0f));
                }
                fc = std::clamp(fc, 20.0f, ctx.sample_rate * 0.49f);
                voice->filter_fc = fc;
                voice->filter_q = zone.filter_q;
                voice->filter_z1 = 0.0f;
                voice->filter_z2 = 0.0f;
                voice->filter_active = (fc < 19000.0f);

                if (voice->filter_active) {
                    // Compute SVF coefficients once at note-on
                    // Q: SF2 stores as dB (0-96). Convert to linear Q factor.
                    // Q_linear = 10^(dB/20), minimum 0.5 to prevent instability
                    float q_db = std::max(1.0f, zone.filter_q);
                    float q_linear = std::max(0.5f, std::pow(10.0f, q_db / 20.0f));

                    float g = std::tan(PI * fc / ctx.sample_rate);
                    float k = 1.0f / q_linear;
                    voice->filter_a1 = 1.0f / (1.0f + g * (g + k));
                    voice->filter_a2 = g * voice->filter_a1;
                    voice->filter_a3 = g * voice->filter_a2;
                }
            }
        }

        // Note off: release all voices for this note
        if (gate_off) {
            state.release_note(note);
        }

        // Mix all active voices
        float output = 0.0f;

        for (std::uint16_t v = 0; v < SoundFontVoiceState::MAX_VOICES; ++v) {
            SFVoice& voice = state.voices[v];
            if (!voice.active) continue;

            // Process envelope
            float env = voice.env_level;
            switch (voice.env_stage) {
                case SFVoice::EnvStage::Idle:
                    env = 0.0f;
                    break;
                case SFVoice::EnvStage::Delay:
                    env = 0.0f;
                    voice.env_time += inv_sr;
                    if (voice.env_time >= voice.env_delay) {
                        voice.env_stage = SFVoice::EnvStage::Attack;
                        voice.env_time = 0.0f;
                    }
                    break;
                case SFVoice::EnvStage::Attack:
                    voice.env_time += inv_sr;
                    env = voice.env_time / voice.env_attack;
                    if (env >= 1.0f) {
                        env = 1.0f;
                        voice.env_stage = SFVoice::EnvStage::Hold;
                        voice.env_time = 0.0f;
                    }
                    break;
                case SFVoice::EnvStage::Hold:
                    env = 1.0f;
                    voice.env_time += inv_sr;
                    if (voice.env_time >= voice.env_hold) {
                        voice.env_stage = SFVoice::EnvStage::Decay;
                        voice.env_time = 0.0f;
                    }
                    break;
                case SFVoice::EnvStage::Decay: {
                    voice.env_time += inv_sr;
                    // Exponential decay toward sustain
                    float decay_progress = voice.env_time / voice.env_decay;
                    if (decay_progress >= 1.0f) {
                        env = voice.env_sustain;
                        voice.env_stage = SFVoice::EnvStage::Sustain;
                    } else {
                        // Exponential interpolation: 1 → sustain
                        float t = 1.0f - std::exp(-5.0f * decay_progress); // ~5 time constants
                        env = 1.0f + (voice.env_sustain - 1.0f) * t;
                    }
                    break;
                }
                case SFVoice::EnvStage::Sustain:
                    env = voice.env_sustain;
                    break;
                case SFVoice::EnvStage::Release: {
                    voice.env_time += inv_sr;
                    float release_progress = voice.env_time / voice.env_release;
                    if (release_progress >= 1.0f) {
                        env = 0.0f;
                        voice.active = false;
                    } else {
                        // Single exponential decay from env_release_start_level
                        // (captured at gate-off). See the matching block in
                        // process_sf_voice_one_sample for why we don't read
                        // env_level here.
                        env = voice.env_release_start_level * std::exp(-5.0f * release_progress);
                    }
                    break;
                }
            }
            voice.env_level = env;

            // Skip if envelope is essentially silent
            if (env < 1e-6f && voice.env_stage == SFVoice::EnvStage::Release) {
                voice.active = false;
                continue;
            }

            // Get sample
            const SampleData* sample = sample_bank->get_sample(voice.sample_id);
            if (!sample || sample->frames == 0) {
                voice.active = false;
                continue;
            }

            // Read sample with interpolation
            float sample_value = 0.0f;
            float pos = voice.position;

            if (pos >= 0.0f && pos < static_cast<float>(voice.sample_end)) {
                // Mix all channels to mono
                for (std::uint32_t ch = 0; ch < sample->channels; ++ch) {
                    sample_value += sample->get_interpolated(pos, ch);
                }
                sample_value /= static_cast<float>(sample->channels);
            }

            // Apply per-voice SVF lowpass filter
            if (voice.filter_active) {
                float v3 = sample_value - voice.filter_z2;
                float v1 = voice.filter_a1 * voice.filter_z1 + voice.filter_a2 * v3;
                float v2 = voice.filter_z2 + voice.filter_a2 * voice.filter_z1 + voice.filter_a3 * v3;
                voice.filter_z1 = 2.0f * v1 - voice.filter_z1;
                voice.filter_z2 = 2.0f * v2 - voice.filter_z2;
                sample_value = v2;  // Lowpass output
            }

            // Apply gain: envelope * attenuation * velocity
            float gain = env * voice.attenuation_linear * voice.velocity_gain;

            // Apply voice-stealing fade
            if (voice.fade_counter < SFVoice::FADE_SAMPLES) {
                gain *= static_cast<float>(voice.fade_counter) / static_cast<float>(SFVoice::FADE_SAMPLES);
                voice.fade_counter++;
            }

            output += sample_value * gain;

            // Advance playback position
            voice.position += voice.speed;

            // Handle looping
            if (voice.loop_mode == 1 || (voice.loop_mode == 3 && !voice.releasing)) {
                // Continuous loop or sustain loop (while not releasing)
                if (voice.loop_end > voice.loop_start) {
                    float loop_len = static_cast<float>(voice.loop_end - voice.loop_start);
                    while (voice.position >= static_cast<float>(voice.loop_end)) {
                        voice.position -= loop_len;
                    }
                }
            }

            // Check if sample finished (for non-looped or post-release)
            if (voice.position >= static_cast<float>(voice.sample_end)) {
                voice.active = false;
            }
        }

        out[i] = output;
    }
}

}  // namespace cedar
