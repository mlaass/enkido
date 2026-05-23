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

// Phase 2 — overlay one closure-produced field value onto an event in place.
// `slot` is an EVENT_OUT_* index; `v` is the closure's returned value for that
// field. NOTE is a coupled rewrite: the closure returns the new *absolute*
// MIDI note, and the per-voice notes[]/values[] (Hz) shift by the same delta
// so a transposed chord stays coherent (primary-voice driven, PRD OQ-1).
[[gnu::always_inline]]
inline void overlay_event_field(OutputEvents::OutputEvent& e,
                                std::uint8_t slot, float v) {
    switch (slot) {
        case EVENT_OUT_NOTE: {
            const float delta = v - e.midi_note;
            const float ratio = std::pow(2.0f, delta / 12.0f);
            e.midi_note = v;
            for (std::uint8_t k = 0; k < e.num_values; ++k) {
                e.notes[k] += delta;
                e.values[k] *= ratio;
            }
            break;
        }
        case EVENT_OUT_VEL:
            e.velocity = v;
            for (std::uint8_t k = 0; k < e.num_values; ++k) e.velocities[k] = v;
            break;
        case EVENT_OUT_DUR:    e.duration = v; break;
        case EVENT_OUT_TIME:   e.time     = v; break;
        case EVENT_OUT_CHANCE: e.chance   = v; break;
        case EVENT_OUT_BEND:
            e.prop_vals[0] = v;
            e.prop_set_mask = static_cast<std::uint8_t>(e.prop_set_mask | 0x01u);
            break;
        case EVENT_OUT_AT:
            e.prop_vals[1] = v;
            e.prop_set_mask = static_cast<std::uint8_t>(e.prop_set_mask | 0x02u);
            break;
        default: break;
    }
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
// EVENT_RATE_SCALE — per-block cycle_length modulator for fast / slow
// ============================================================================
//
// PRD prd-runtime-event-transforms.md, Phase 3. Unlike EVENT_MAP / EVENT_FILTER
// (which iterate events), this opcode mutates a SequenceState in place: each
// block it sets `upstream.cycle_length = original_cycle_length / rate`. A
// single-field write covers every SEQPAT_* opcode (QUERY/STEP/GATE/FIELD/
// PHASE/TYPE/PROP/VALUES) — they all consult `state.cycle_length` for cycle
// position and event scaling — so no external-clock plumbing is needed.
//
//   inputs[0]: BUFFER_UNUSED.
//   inputs[1]: rate factor — constant buffer or signal-rate. Clamped to
//              >= 0.001 to avoid frozen / reversed playback. For `slow`,
//              codegen emits the reciprocal (factor → 1/factor) so the
//              opcode itself only knows about `rate`.
//   inputs[2..3]: upstream SequenceState state_id (packed low/high 16 bits,
//                same encoding as EVENT_MAP / EVENT_FILTER).
//   inputs[4]: BUFFER_UNUSED.
//   out_buffer: BUFFER_UNUSED (no signal output).
//   state_id: RateScaleState.
//
// Composition: compile-time fast/slow nesting accumulates the original
// cycle_length via compile_pattern_for_transform's recursive path. ERS
// captures that compile-time-accumulated cycle_length on first block and
// applies the OUTER factor on top each block. Nested signal-rate is not
// supported in Phase 3 (compile_pattern_for_transform rejects non-constant
// nested factors).

[[gnu::always_inline]]
inline void op_event_rate_scale(ExecutionContext& ctx, const Instruction& inst) {
    auto& state = ctx.states->get_or_create<RateScaleState>(inst.state_id);

    const std::uint32_t upstream_id = event_transform_upstream_id(inst);
    auto* upstream = ctx.states->get_if<SequenceState>(upstream_id);
    if (!upstream) return;  // upstream not yet realised; SEQPAT_QUERY guards

    if (!state.initialized) {
        state.original_cycle_length = upstream->cycle_length;
        state.initialized = true;
    }

    float rate = 1.0f;
    if (inst.inputs[1] != BUFFER_UNUSED) {
        rate = ctx.buffers->get(inst.inputs[1])[0];
    }
    if (!(rate >= 0.001f)) rate = 0.001f;  // NaN-safe; avoids frozen/reversed playback

    upstream->cycle_length = state.original_cycle_length / rate;
}

// ============================================================================
// EVENT_REORDER — structural transforms (rev/palindrome/iter/zoom/compress)
// ============================================================================
//
// PRD prd-runtime-event-transforms.md, Phase 4. Reads upstream OutputEvents,
// dispatches on `inst.rate & 0x0F` (see event_transform_encoding.hpp), writes
// the transformed events into a transform-owned SequenceState. Composes with
// upstream EVENT_MAP / EVENT_FILTER / EVENT_RATE_SCALE because every transform
// chain hands off via OutputEvents.
//
//   rate (u8):  bits 0-3 = kind (EVENT_REORDER_*); bits 4-7 = per-kind flags.
//   inputs[0..1]: kind-dependent params (see per-kind comments).
//   inputs[2..3]: upstream state_id (packed low/high; same as EVENT_MAP).
//   out_buffer:  BUFFER_UNUSED.  state_id: transform-owned SequenceState.

// REV: new_time = cycle_length - t - dur (clamp >= 0). 1x capacity.
[[gnu::always_inline]]
inline void reorder_rev(SequenceState& dst, const OutputEvents& src) {
    const float cyc = dst.cycle_length;
    const std::uint32_t n = std::min(src.num_events, dst.output.capacity);
    for (std::uint32_t i = 0; i < n; ++i) {
        OutputEvents::OutputEvent e = src.events[i];
        float nt = cyc - e.time - e.duration;
        if (nt < 0.0f) nt = 0.0f;
        e.time = nt;
        dst.output.events[i] = e;
    }
    dst.output.num_events = n;
}

// PALINDROME: each event emitted twice — forward at (0.5*t, 0.5*dur) and
// reverse at (1 - 0.5*t - 0.5*dur, 0.5*dur). Downstream cycle_length is
// 2x the upstream's. Capacity guard: ceil( capacity / 2 ) input events.
[[gnu::always_inline]]
inline void reorder_palindrome(SequenceState& dst, const OutputEvents& src) {
    std::uint32_t w = 0;
    for (std::uint32_t i = 0; i < src.num_events && w + 1 < dst.output.capacity; ++i) {
        OutputEvents::OutputEvent e = src.events[i];
        OutputEvents::OutputEvent r = e;
        e.time = e.time * 0.5f;
        e.duration = e.duration * 0.5f;
        dst.output.events[w++] = e;
        float rt = 1.0f - 0.5f * r.time - 0.5f * r.duration;
        if (rt < 0.0f) rt = 0.0f;
        r.time = rt;
        r.duration = r.duration * 0.5f;
        dst.output.events[w++] = r;
    }
    dst.output.num_events = w;
    // cycle_length doubles regardless of input event count.
    dst.cycle_length = (dst.reorder_original_cycle_length > 0.0f
                           ? dst.reorder_original_cycle_length
                           : 1.0f) * 2.0f;
}

// ZOOM(start, end): keep events overlapping [s, e), rescale to [0, 1).
// Degenerate (e <= s) -> empty.
[[gnu::always_inline]]
inline void reorder_zoom(ExecutionContext& ctx, const Instruction& inst,
                         SequenceState& dst, const OutputEvents& src) {
    const float s = (inst.inputs[0] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[0])[0] : 0.0f;
    const float e = (inst.inputs[1] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[1])[0] : 1.0f;
    if (!(e > s)) {
        dst.output.num_events = 0;
        return;
    }
    const float width = e - s;
    std::uint32_t w = 0;
    for (std::uint32_t i = 0; i < src.num_events && w < dst.output.capacity; ++i) {
        OutputEvents::OutputEvent ev = src.events[i];
        const float ev_end = ev.time + ev.duration;
        if (ev_end <= s || ev.time >= e) continue;
        const float cs = std::max(ev.time, s);
        const float ce = std::min(ev_end, e);
        ev.time = (cs - s) / width;
        ev.duration = (ce - cs) / width;
        dst.output.events[w++] = ev;
    }
    dst.output.num_events = w;
}

// ITER / ITER_BACK(n): rotate upstream events by -dir * (cycle % n) / n of
// the cycle each cycle. Migrated from the legacy SEQPAT_QUERY iter_n/iter_dir
// rotation block in Commit C of PRD prd-runtime-event-transforms Phase 4.
[[gnu::always_inline]]
inline void reorder_iter(ExecutionContext& ctx, const Instruction& inst,
                         SequenceState& dst, const OutputEvents& src) {
    const std::uint8_t kind = inst.rate & 0x0F;
    const std::int8_t  dir  = (kind == EVENT_REORDER_ITER_BACK ||
                               ((inst.rate >> 4) & EVENT_REORDER_ITER_DIR_BACK))
                                  ? -1 : +1;

    std::uint32_t n_int = 1;
    if (inst.inputs[0] != BUFFER_UNUSED) {
        float n_f = ctx.buffers->get(inst.inputs[0])[0];
        if (n_f >= 1.0f) n_int = static_cast<std::uint32_t>(n_f);
    }
    if (n_int < 1) n_int = 1;

    const float spc = ctx.samples_per_beat();   // 1 cycle == 1 beat post-revert
    const float upstream_cyc = (dst.reorder_original_cycle_length > 0.0f)
                                  ? dst.reorder_original_cycle_length : 1.0f;
    const float denom = spc * upstream_cyc;
    const std::uint32_t cycle_index = (denom > 0.0f)
        ? static_cast<std::uint32_t>(std::floor(
              static_cast<float>(ctx.global_sample_counter) / denom))
        : 0u;

    const std::uint32_t k = cycle_index % n_int;
    const float shift = -static_cast<float>(dir) *
                        static_cast<float>(k) * upstream_cyc /
                        static_cast<float>(n_int);

    const std::uint32_t copies = std::min(src.num_events, dst.output.capacity);
    for (std::uint32_t i = 0; i < copies; ++i) {
        OutputEvents::OutputEvent e = src.events[i];
        float t = e.time + shift;
        t = std::fmod(t, upstream_cyc);
        if (t < 0.0f) t += upstream_cyc;
        e.time = t;
        dst.output.events[i] = e;
    }
    dst.output.num_events = copies;
    dst.cycle_length = upstream_cyc;
    dst.last_reorder_cycle = cycle_index;
}

// COMPRESS(start, end): rescale source's [0, 1) cycle phase into [s, e).
// Degenerate (e <= s) -> empty.
[[gnu::always_inline]]
inline void reorder_compress(ExecutionContext& ctx, const Instruction& inst,
                             SequenceState& dst, const OutputEvents& src) {
    const float s = (inst.inputs[0] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[0])[0] : 0.0f;
    const float e = (inst.inputs[1] != BUFFER_UNUSED)
        ? ctx.buffers->get(inst.inputs[1])[0] : 1.0f;
    if (!(e > s)) {
        dst.output.num_events = 0;
        return;
    }
    const float width = e - s;
    const std::uint32_t n = std::min(src.num_events, dst.output.capacity);
    for (std::uint32_t i = 0; i < n; ++i) {
        OutputEvents::OutputEvent ev = src.events[i];
        ev.time = s + ev.time * width;
        ev.duration = ev.duration * width;
        dst.output.events[i] = ev;
    }
    dst.output.num_events = n;
}

[[gnu::always_inline]]
inline void op_event_reorder(ExecutionContext& ctx, const Instruction& inst) {
    auto& dst = ctx.states->get_or_create<SequenceState>(inst.state_id);

    auto src = ctx.states->resolve_output_events(event_transform_upstream_id(inst));
    if (!src.events) {
        dst.output.clear();
        dst.current_index = 0;
        return;
    }

    // Capture upstream cycle_length once. Used by PALINDROME (downstream
    // cycle_length = original * 2) and later by ITER / LINGER. Captured
    // before we mutate dst.cycle_length so the snapshot reflects the
    // upstream's true cycle.
    if (!dst.reorder_captured) {
        dst.reorder_original_cycle_length = src.cycle_length;
        dst.reorder_captured = true;
    }
    dst.cycle_length = src.cycle_length;  // most kinds inherit; PALINDROME overrides

    const std::uint8_t kind = inst.rate & 0x0F;
    dst.output.clear();

    switch (kind) {
        case EVENT_REORDER_REV:        reorder_rev(dst, *src.events);                      break;
        case EVENT_REORDER_PALINDROME: reorder_palindrome(dst, *src.events);               break;
        case EVENT_REORDER_ITER:       /* fallthrough */
        case EVENT_REORDER_ITER_BACK:  reorder_iter(ctx, inst, dst, *src.events);          break;
        case EVENT_REORDER_ZOOM:       reorder_zoom(ctx, inst, dst, *src.events);          break;
        case EVENT_REORDER_COMPRESS:   reorder_compress(ctx, inst, dst, *src.events);      break;
        default: break;
    }

    dst.output.sort_by_time();
    event_transform_reposition(dst, ctx);
}

// ============================================================================
// EVENT_FANOUT — structural transforms (ply / linger / segment)
// ============================================================================
//
// PRD prd-runtime-event-transforms.md, Phase 4 Commit B. Same shape as
// EVENT_REORDER: read upstream OutputEvents, switch on kind, write into a
// transform-owned SequenceState. The opcode handles transforms that change
// event count: PLY duplicates each event N times; LINGER drops events with
// t >= frac and rescales survivors; SEGMENT samples N grid points across
// the cycle.

// PLY(n): each event -> n sub-events at (t + k*d/n, d/n).
[[gnu::always_inline]]
inline void fanout_ply(ExecutionContext& ctx, const Instruction& inst,
                       SequenceState& dst, const OutputEvents& src) {
    std::uint32_t n_int = 1;
    if (inst.inputs[0] != BUFFER_UNUSED) {
        float n_f = ctx.buffers->get(inst.inputs[0])[0];
        if (n_f >= 1.0f) n_int = static_cast<std::uint32_t>(n_f);
    }
    if (n_int < 1) n_int = 1;
    std::uint32_t w = 0;
    for (std::uint32_t i = 0; i < src.num_events && w < dst.output.capacity; ++i) {
        const auto& base = src.events[i];
        const float sub_d = base.duration / static_cast<float>(n_int);
        for (std::uint32_t k = 0; k < n_int && w < dst.output.capacity; ++k) {
            OutputEvents::OutputEvent ne = base;
            ne.duration = sub_d;
            ne.time = base.time + static_cast<float>(k) * sub_d;
            dst.output.events[w++] = ne;
        }
    }
    dst.output.num_events = w;
}

// LINGER(frac): drop events with t >= frac, rescale survivors to [0, 1);
// clamp dur so events don't overshoot the cycle. Downstream cycle_length is
// upstream * frac (so frac < 1 makes the segment LOOP frac^-1 times within
// the upstream cycle, matching the legacy compile-time semantics).
[[gnu::always_inline]]
inline void fanout_linger(ExecutionContext& ctx, const Instruction& inst,
                          SequenceState& dst, const OutputEvents& src) {
    float frac = (inst.inputs[0] != BUFFER_UNUSED)
                    ? ctx.buffers->get(inst.inputs[0])[0] : 1.0f;
    if (!(frac > 0.0f)) frac = 0.001f;   // NaN-safe; avoids div/0
    if (frac > 1.0f) frac = 1.0f;        // PRD: frac >= 1 is a no-op cap
    std::uint32_t w = 0;
    for (std::uint32_t i = 0; i < src.num_events && w < dst.output.capacity; ++i) {
        const auto& base = src.events[i];
        if (base.time >= frac) continue;
        OutputEvents::OutputEvent ne = base;
        ne.time = base.time / frac;
        ne.duration = base.duration / frac;
        if (ne.time + ne.duration > 1.0f) ne.duration = 1.0f - ne.time;
        if (ne.duration < 0.0f) continue;
        dst.output.events[w++] = ne;
    }
    dst.output.num_events = w;
    const float original = (dst.reorder_original_cycle_length > 0.0f)
                              ? dst.reorder_original_cycle_length : 1.0f;
    dst.cycle_length = original * frac;
}

// SEGMENT(n): sample n equal grid points across the cycle; each point picks
// the active upstream event at that time (or the latest preceding event if
// none is active). The output is N events each of duration 1/N.
[[gnu::always_inline]]
inline void fanout_segment(ExecutionContext& ctx, const Instruction& inst,
                           SequenceState& dst, const OutputEvents& src) {
    std::uint32_t n_int = 1;
    if (inst.inputs[0] != BUFFER_UNUSED) {
        float n_f = ctx.buffers->get(inst.inputs[0])[0];
        if (n_f >= 1.0f) n_int = static_cast<std::uint32_t>(n_f);
    }
    if (n_int < 1) n_int = 1;
    const float step = 1.0f / static_cast<float>(n_int);
    std::uint32_t w = 0;
    for (std::uint32_t k = 0; k < n_int && w < dst.output.capacity; ++k) {
        const float st = static_cast<float>(k) * step;
        const OutputEvents::OutputEvent* active = nullptr;
        // Upstream events are time-sorted (SEQPAT_QUERY / EVENT_MAP guarantee
        // this when they touch the TIME field). Walk once, keep the latest
        // event whose [t, t+dur) covers st; fall back to the latest event
        // with t <= st.
        for (std::uint32_t i = 0; i < src.num_events; ++i) {
            const auto& ev = src.events[i];
            if (ev.time <= st && (ev.time + ev.duration) > st) {
                active = &ev;
            } else if (ev.time <= st) {
                active = &ev;
            }
        }
        if (!active && src.num_events > 0) active = &src.events[0];
        if (!active) continue;
        OutputEvents::OutputEvent ne = *active;
        ne.time = st;
        ne.duration = step;
        dst.output.events[w++] = ne;
    }
    dst.output.num_events = w;
}

[[gnu::always_inline]]
inline void op_event_fanout(ExecutionContext& ctx, const Instruction& inst) {
    auto& dst = ctx.states->get_or_create<SequenceState>(inst.state_id);

    auto src = ctx.states->resolve_output_events(event_transform_upstream_id(inst));
    if (!src.events) {
        dst.output.clear();
        dst.current_index = 0;
        return;
    }

    if (!dst.reorder_captured) {
        dst.reorder_original_cycle_length = src.cycle_length;
        dst.reorder_captured = true;
    }
    dst.cycle_length = src.cycle_length;  // PLY/SEGMENT inherit; LINGER overrides

    const std::uint8_t kind = inst.rate & 0x0F;
    dst.output.clear();

    switch (kind) {
        case EVENT_FANOUT_PLY:     fanout_ply(ctx, inst, dst, *src.events);     break;
        case EVENT_FANOUT_LINGER:  fanout_linger(ctx, inst, dst, *src.events);  break;
        case EVENT_FANOUT_SEGMENT: fanout_segment(ctx, inst, dst, *src.events); break;
        default: break;
    }

    dst.output.sort_by_time();
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
