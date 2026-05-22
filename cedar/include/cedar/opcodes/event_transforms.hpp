#pragma once

#include "../vm/context.hpp"
#include "../vm/instruction.hpp"
#include "../dsp/constants.hpp"
#include "dsp_state.hpp"
#include "sequence.hpp"
#include "event_transform_encoding.hpp"
#include <cmath>
#include <algorithm>

namespace cedar {

// ============================================================================
// Runtime Event-Stream Transforms — EVENT_MAP / EVENT_FILTER
// ============================================================================
//
// PRD prd-runtime-event-transforms.md, Phase 1. These opcodes operate on the
// OutputEvents wire format: they read an upstream event source's OutputEvents
// (a SequenceState pattern OR a MidiQueueState — resolved uniformly via
// StatePool::resolve_output_events) and publish their own transformed
// OutputEvents into a transform-owned SequenceState. Downstream SEQPAT_STEP /
// SEQPAT_FIELD / SEQPAT_GATE read that SequenceState unchanged.
//
// They are plain per-block transforms — regular execute() switch cases, NOT
// dispatch-loop opcodes. They never alter `ip`.
//
// Instruction encoding (Phase-1 scaffolding — Phase 2 replaces the rate
// packing with a closure block_id):
//   rate (u8): bits 0-3 = field selector, bits 4-5 = op / comparison
//   inputs[0]: buffer holding the constant parameter (read at sample 0;
//              block-uniform in Phase 1). 0xFFFF = parameter 0.
//   inputs[1]: 0xFFFF (reserved for a signal-rate parameter in Phase 3+)
//   inputs[2]/inputs[3]: low / high 16 bits of the upstream state_id
//   inputs[4]: 0xFFFF.  out_buffer: 0xFFFF (writes no signal buffer)
//   state_id: the transform-owned downstream SequenceState ("B")

// Apply a Phase-1 constant transform to a single event in place.
[[gnu::always_inline]]
inline void apply_event_transform(OutputEvents::OutputEvent& e,
                                  std::uint8_t field, std::uint8_t op, float param) {
    auto scalar = [op](float v, float p) -> float {
        switch (op) {
            case EVENT_OP_ADD: return v + p;
            case EVENT_OP_MUL: return v * p;
            case EVENT_OP_SET: return p;
            default:           return v;
        }
    };
    switch (field) {
        case EVENT_FIELD_NOTE_COUPLED: {
            // transpose by `param` semitones — coupled rewrite of the MIDI
            // note fields and the per-voice frequency values. `op` is ignored:
            // NOTE_COUPLED is intrinsically an additive semitone shift.
            const float ratio = std::pow(2.0f, param / 12.0f);
            e.midi_note += param;
            for (std::uint8_t v = 0; v < e.num_values; ++v) {
                e.notes[v] += param;
                e.values[v] *= ratio;
            }
            break;
        }
        case EVENT_FIELD_VEL: {
            e.velocity = scalar(e.velocity, param);
            for (std::uint8_t v = 0; v < e.num_values; ++v) {
                e.velocities[v] = scalar(e.velocities[v], param);
            }
            break;
        }
        case EVENT_FIELD_DUR:    e.duration = scalar(e.duration, param); break;
        case EVENT_FIELD_TIME:   e.time     = scalar(e.time, param);     break;
        case EVENT_FIELD_CHANCE: e.chance   = scalar(e.chance, param);   break;
        default: break;
    }
}

// Read the scalar value of `field` from an event (EVENT_FILTER predicate).
[[gnu::always_inline]]
inline float event_field_value(const OutputEvents::OutputEvent& e,
                                std::uint8_t field) {
    switch (field) {
        case EVENT_FIELD_NOTE_COUPLED: return e.midi_note;
        case EVENT_FIELD_VEL:          return e.velocity;
        case EVENT_FIELD_DUR:          return e.duration;
        case EVENT_FIELD_TIME:         return e.time;
        case EVENT_FIELD_CHANCE:       return e.chance;
        default:                       return 0.0f;
    }
}

// Re-establish current_index for a transform-owned SequenceState, mirroring
// the tail of op_seqpat_query: advance past every event earlier than the
// current cycle position. EVENT_MAP/EVENT_FILTER rebuild output every block,
// so current_index must be re-derived every block to stay consistent with
// the per-sample beat tracking in downstream SEQPAT_STEP.
[[gnu::always_inline]]
inline void event_transform_reposition(SequenceState& state,
                                       ExecutionContext& ctx) {
    state.current_index = 0;
    if (state.cycle_length <= 0.0f) return;
    const float spb = ctx.samples_per_beat();
    const float beat_start = static_cast<float>(ctx.global_sample_counter) / spb;
    const float cycle_pos = std::fmod(beat_start, state.cycle_length);
    while (state.current_index < state.output.num_events &&
           state.output.events[state.current_index].time < cycle_pos) {
        state.current_index++;
    }
}

// Decode the 32-bit upstream state_id packed across inputs[2]+inputs[3].
[[gnu::always_inline]]
inline std::uint32_t event_transform_upstream_id(const Instruction& inst) {
    return static_cast<std::uint32_t>(inst.inputs[2]) |
           (static_cast<std::uint32_t>(inst.inputs[3]) << 16);
}

// ============================================================================
// EVENT_MAP — per-event field rewrite
// ============================================================================
[[gnu::always_inline]]
inline void op_event_map(ExecutionContext& ctx, const Instruction& inst) {
    auto& dst = ctx.states->get_or_create<SequenceState>(inst.state_id);

    auto src = ctx.states->resolve_output_events(event_transform_upstream_id(inst));
    if (!src.events) {
        dst.output.clear();
        dst.current_index = 0;
        return;
    }

    dst.cycle_length = src.cycle_length;

    const std::uint8_t field = inst.rate & 0x0F;
    const std::uint8_t op    = static_cast<std::uint8_t>((inst.rate >> 4) & 0x03);
    const float param = (inst.inputs[0] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[0])[0]
        : 0.0f;

    dst.output.clear();
    const std::uint32_t n = std::min(src.events->num_events, dst.output.capacity);
    for (std::uint32_t i = 0; i < n; ++i) {
        OutputEvents::OutputEvent e = src.events->events[i];  // struct copy
        apply_event_transform(e, field, op, param);
        dst.output.events[i] = e;
    }
    dst.output.num_events = n;

    // Only a TIME rewrite can reorder events; otherwise time is untouched.
    if (field == EVENT_FIELD_TIME) {
        dst.output.sort_by_time();
    }

    event_transform_reposition(dst, ctx);
}

// ============================================================================
// EVENT_FILTER — predicate drop
// ============================================================================
[[gnu::always_inline]]
inline void op_event_filter(ExecutionContext& ctx, const Instruction& inst) {
    auto& dst = ctx.states->get_or_create<SequenceState>(inst.state_id);

    auto src = ctx.states->resolve_output_events(event_transform_upstream_id(inst));
    if (!src.events) {
        dst.output.clear();
        dst.current_index = 0;
        return;
    }

    dst.cycle_length = src.cycle_length;

    const std::uint8_t field = inst.rate & 0x0F;
    const std::uint8_t cmp   = static_cast<std::uint8_t>((inst.rate >> 4) & 0x03);
    const float threshold = (inst.inputs[0] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[0])[0]
        : 0.0f;

    dst.output.clear();
    std::uint32_t written = 0;
    for (std::uint32_t i = 0;
         i < src.events->num_events && written < dst.output.capacity; ++i) {
        const auto& e = src.events->events[i];
        const float fv = event_field_value(e, field);
        const bool keep = (cmp == EVENT_CMP_LTE) ? (fv <= threshold)
                                                 : (fv >= threshold);
        if (keep) {
            dst.output.events[written++] = e;
        }
    }
    dst.output.num_events = written;

    event_transform_reposition(dst, ctx);
}

}  // namespace cedar
