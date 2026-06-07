#pragma once

// State-init buffer codec — pack/apply the post-compile data that
// `cedar_apply_state_inits`, `cedar_apply_midi_sources` and
// `akkado_resolve_sample_ids` used to read from the in-WASM
// `g_compile_result` global, into a self-describing byte stream that can
// travel across the worker → main → worklet boundary.
//
// PRD: docs/prd-compile-off-audio-thread.md §5.
//
// Wire format (little-endian throughout):
//
//   stateInitsBuf:
//     [ magic: u32 = "INIT" ]
//     [ version: u16 = WIRE_VERSION ]
//     [ record_count: u16 ]
//     [ record_0 ] [ record_1 ] ...
//
//   midiSourcesBuf:
//     [ magic: u32 = "MIDI" ]
//     [ version: u16 = WIRE_VERSION ]
//     [ record_count: u16 ]
//     [ record_0 ] [ record_1 ] ...
//
//   blockTable: plain memcpy of cedar::BlockEntry[entry_count].
//
// Record header (12 bytes):
//     [ kind: u8 ]        // 0=StateInit, 1=SequenceSampleMapping batch, 2=ScalarSampleMapping batch
//     [ pad: u8[3] ]
//     [ state_id: u32 ]   // kind=0/1: owning state's id; kind=2: 0
//     [ payload_size: u32 ]
//     [ payload bytes ... ]
//
// Per-payload layouts follow the C++ struct field order exactly. Records
// that embed cedar::Event, cedar::TimelineState::Breakpoint or
// cedar::BlockEntry use memcpy of the struct (worker and worklet share
// the same build → layouts match by construction; the `version` field
// gates the build-mismatch case).

#include <akkado/codegen.hpp>

#include <cedar/vm/vm.hpp>
#include <cedar/vm/program_slot.hpp>
#include <cedar/opcodes/sequence.hpp>
#include <cedar/opcodes/midi.hpp>
#include <cedar/opcodes/dsp_state.hpp>

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace akkado::state_init_buffer {

inline constexpr std::uint32_t MAGIC_STATE_INITS  = 0x494E4954u;  // "INIT"
inline constexpr std::uint32_t MAGIC_MIDI_SOURCES = 0x4D494449u;  // "MIDI"
inline constexpr std::uint16_t WIRE_VERSION       = 1;

// Wire-format size tripwires. The SequenceProgram / Timeline payloads embed
// these structs via raw memcpy, so any size change silently alters the wire
// layout. Pinning here forces a deliberate WIRE_VERSION bump (and a refresh of
// the generated TS validator in web/src/lib/audio/state-init-codec.ts) on the
// same commit that changes a struct. cedar::BlockEntry is pinned at its own
// definition (program_slot.hpp).
static_assert(sizeof(cedar::Event) == 240,
              "cedar::Event size changed — bump WIRE_VERSION and regenerate "
              "the TS state-init codec");
static_assert(sizeof(cedar::TimelineState::Breakpoint) == 12,
              "TimelineState::Breakpoint size changed — bump WIRE_VERSION and "
              "regenerate the TS state-init codec");

enum class RecordKind : std::uint8_t {
    StateInit             = 0,
    SequenceSampleMapping = 1,
    ScalarSampleMapping   = 2,
};

// ---------------------------------------------------------------------------
// Byte writer / reader helpers — unaligned via memcpy for safety.
// ---------------------------------------------------------------------------

namespace detail {

struct Writer {
    std::vector<std::uint8_t>& buf;

    void u8 (std::uint8_t  v) { buf.push_back(v); }
    void u16(std::uint16_t v) { append(&v, sizeof(v)); }
    void u32(std::uint32_t v) { append(&v, sizeof(v)); }
    void i32(std::int32_t  v) { append(&v, sizeof(v)); }
    void f32(float         v) { append(&v, sizeof(v)); }
    void pad(std::size_t n)   { buf.insert(buf.end(), n, 0); }
    void bytes(const void* p, std::size_t n) { append(p, n); }
    void str(std::string_view s) { append(s.data(), s.size()); }

    std::size_t position() const { return buf.size(); }
    void patch_u32(std::size_t at, std::uint32_t v) {
        std::memcpy(buf.data() + at, &v, sizeof(v));
    }

private:
    void append(const void* p, std::size_t n) {
        const auto* src = static_cast<const std::uint8_t*>(p);
        buf.insert(buf.end(), src, src + n);
    }
};

struct Reader {
    const std::uint8_t* p;
    const std::uint8_t* end;

    bool ok() const { return p <= end; }
    std::size_t remaining() const { return static_cast<std::size_t>(end - p); }
    bool need(std::size_t n) const { return remaining() >= n; }

    std::uint8_t  u8 () { std::uint8_t  v = 0; read(&v, 1); return v; }
    std::uint16_t u16() { std::uint16_t v = 0; read(&v, 2); return v; }
    std::uint32_t u32() { std::uint32_t v = 0; read(&v, 4); return v; }
    std::int32_t  i32() { std::int32_t  v = 0; read(&v, 4); return v; }
    float         f32() { float         v = 0; read(&v, 4); return v; }
    void skip(std::size_t n) { p += n; }
    void read(void* dst, std::size_t n) {
        if (!need(n)) { p = end + 1; return; }
        std::memcpy(dst, p, n);
        p += n;
    }
    std::string_view str(std::size_t n) {
        if (!need(n)) { p = end + 1; return {}; }
        std::string_view sv(reinterpret_cast<const char*>(p), n);
        p += n;
        return sv;
    }
};

} // namespace detail

// ---------------------------------------------------------------------------
// Packers
// ---------------------------------------------------------------------------

inline std::vector<std::uint8_t>
pack_state_inits(const std::vector<StateInitData>& state_inits,
                 const std::vector<ScalarSampleMapping>& scalar_mappings) {
    using namespace detail;
    std::vector<std::uint8_t> buf;
    Writer w{buf};

    std::uint16_t record_count = 0;
    record_count += static_cast<std::uint16_t>(state_inits.size());
    for (const auto& init : state_inits) {
        if (init.type == StateInitData::Type::SequenceProgram &&
            !init.sequence_sample_mappings.empty()) {
            ++record_count;
        }
    }
    if (!scalar_mappings.empty()) ++record_count;

    w.u32(MAGIC_STATE_INITS);
    w.u16(WIRE_VERSION);
    w.u16(record_count);

    auto write_record_header = [&](RecordKind kind, std::uint32_t state_id,
                                   std::size_t& payload_size_at) {
        w.u8(static_cast<std::uint8_t>(kind));
        w.pad(3);
        w.u32(state_id);
        payload_size_at = w.position();
        w.u32(0);  // payload_size — patched after payload is written.
    };
    auto finish_record = [&](std::size_t payload_size_at) {
        std::uint32_t payload_size =
            static_cast<std::uint32_t>(w.position() - payload_size_at - 4);
        w.patch_u32(payload_size_at, payload_size);
    };

    for (const auto& init : state_inits) {
        std::size_t hdr_size_at = 0;
        write_record_header(RecordKind::StateInit, init.state_id, hdr_size_at);

        // Payload always starts with the StateInitData::Type discriminant.
        w.u8(static_cast<std::uint8_t>(init.type));
        w.pad(3);

        switch (init.type) {
        case StateInitData::Type::Timeline: {
            w.u8(init.timeline_loop ? 1 : 0);
            w.pad(3);
            w.f32(init.timeline_loop_length);
            w.u32(static_cast<std::uint32_t>(init.timeline_breakpoints.size()));
            if (!init.timeline_breakpoints.empty()) {
                w.bytes(init.timeline_breakpoints.data(),
                        init.timeline_breakpoints.size() *
                            sizeof(cedar::TimelineState::Breakpoint));
            }
            break;
        }
        case StateInitData::Type::SequenceProgram: {
            w.f32(init.cycle_length);
            w.u8(init.is_sample_pattern ? 1 : 0);
            w.pad(3);
            w.u32(init.total_events);
            w.u32(static_cast<std::uint32_t>(init.sequences.size()));
            for (std::size_t i = 0; i < init.sequences.size(); ++i) {
                const auto& seq = init.sequences[i];
                w.f32(seq.duration);
                w.u8(static_cast<std::uint8_t>(seq.mode));
                w.pad(3);
                std::uint32_t nev = 0;
                if (i < init.sequence_events.size()) {
                    nev = static_cast<std::uint32_t>(init.sequence_events[i].size());
                }
                w.u32(nev);
                if (nev > 0) {
                    w.bytes(init.sequence_events[i].data(),
                            nev * sizeof(cedar::Event));
                }
            }
            break;
        }
        case StateInitData::Type::PolyAlloc: {
            w.u32(init.poly_seq_state_id);
            w.u8(init.poly_max_voices);
            w.u8(init.poly_mode);
            w.u8(init.poly_steal_strategy);
            w.u8(init.poly_prop_count);
            w.f32(init.poly_release_seconds);
            w.bytes(init.poly_prop_defaults,
                    cedar::MAX_PROPS_PER_EVENT * sizeof(float));
            break;
        }
        case StateInitData::Type::ExtendedParams: {
            w.u8(init.ext_count);
            w.pad(3);
            w.bytes(init.ext_constants.data(),
                    init.ext_constants.size() * sizeof(float));
            w.bytes(init.ext_buffer_indices.data(),
                    init.ext_buffer_indices.size() * sizeof(std::uint16_t));
            break;
        }
        case StateInitData::Type::SoundfontEvents: {
            w.u32(init.sf_seq_state_id);
            w.i32(init.sf_preset_idx);
            break;
        }
        case StateInitData::Type::ForeachAlloc: {
            w.u8(init.foreach_allocator_kind);
            w.pad(3);
            w.u32(init.foreach_block_id);
            w.u32(init.foreach_event_src_state_id);
            w.u16(init.foreach_max_iterations);
            w.pad(2);
            w.u8(init.poly_max_voices);
            w.u8(init.poly_mode);
            w.u8(init.poly_steal_strategy);
            w.u8(init.poly_prop_count);
            w.f32(init.poly_release_seconds);
            w.bytes(init.poly_prop_defaults,
                    cedar::MAX_PROPS_PER_EVENT * sizeof(float));
            break;
        }
        case StateInitData::Type::EventTransform:
        case StateInitData::Type::Reorder:
        case StateInitData::Type::Fanout: {
            w.f32(init.cycle_length);
            w.u8(init.is_sample_pattern ? 1 : 0);
            w.pad(3);
            w.u32(init.total_events);
            break;
        }
        case StateInitData::Type::RateScale:
            // No further payload.
            break;
        }

        finish_record(hdr_size_at);
    }

    for (const auto& init : state_inits) {
        if (init.type != StateInitData::Type::SequenceProgram) continue;
        if (init.sequence_sample_mappings.empty()) continue;

        std::size_t hdr_size_at = 0;
        write_record_header(RecordKind::SequenceSampleMapping,
                            init.state_id, hdr_size_at);
        w.u32(static_cast<std::uint32_t>(init.sequence_sample_mappings.size()));
        for (const auto& m : init.sequence_sample_mappings) {
            w.u16(m.seq_idx);
            w.u16(m.event_idx);
            w.u8(m.value_slot);
            w.u8(m.variant);
            w.pad(2);
            w.u16(static_cast<std::uint16_t>(m.bank.size()));
            w.u16(static_cast<std::uint16_t>(m.sample_name.size()));
            w.str(m.bank);
            w.str(m.sample_name);
        }
        finish_record(hdr_size_at);
    }

    if (!scalar_mappings.empty()) {
        std::size_t hdr_size_at = 0;
        write_record_header(RecordKind::ScalarSampleMapping, 0, hdr_size_at);
        w.u32(static_cast<std::uint32_t>(scalar_mappings.size()));
        for (const auto& m : scalar_mappings) {
            w.u32(m.instruction_index);
            w.u8(static_cast<std::uint8_t>(m.variant));
            w.pad(3);
            w.u16(static_cast<std::uint16_t>(m.bank.size()));
            w.u16(static_cast<std::uint16_t>(m.name.size()));
            w.str(m.bank);
            w.str(m.name);
        }
        finish_record(hdr_size_at);
    }

    return buf;
}

inline std::vector<std::uint8_t>
pack_midi_sources(const std::vector<RequiredMidiSource>& sources) {
    using namespace detail;
    std::vector<std::uint8_t> buf;
    Writer w{buf};

    w.u32(MAGIC_MIDI_SOURCES);
    w.u16(WIRE_VERSION);
    w.u16(static_cast<std::uint16_t>(sources.size()));

    for (const auto& src : sources) {
        w.u32(src.state_id);
        w.u8(static_cast<std::uint8_t>(src.kind));
        w.u8(src.channel_filter);
        w.u8(src.loop ? 1 : 0);
        w.u8(static_cast<std::uint8_t>(src.tempo_mode));
        w.u16(static_cast<std::uint16_t>(src.name_or_path.size()));
        w.pad(2);
        w.str(src.name_or_path);
        // 4-byte align the record so the next record's u32 fields stay
        // naturally aligned in the buffer. Pads 0..3 bytes.
        while (w.position() % 4 != 0) w.u8(0);
    }
    return buf;
}

inline std::vector<std::uint8_t>
pack_block_table(const std::vector<cedar::BlockEntry>& entries) {
    std::vector<std::uint8_t> buf;
    if (entries.empty()) return buf;
    buf.resize(entries.size() * sizeof(cedar::BlockEntry));
    std::memcpy(buf.data(), entries.data(), buf.size());
    return buf;
}

// ---------------------------------------------------------------------------
// Apply (unpack into a VM)
// ---------------------------------------------------------------------------

// Lookup name format mirrors the legacy g_compile_result-driven apply paths
// (akkado_resolve_sample_ids, akkado_patch_sample_ids_in_bytecode):
//   default bank: "name" or "name:variant" (when variant > 0)
//   named bank:   "bank_name_variant"
inline std::string make_sample_lookup_name(std::string_view bank,
                                           std::string_view name,
                                           std::uint8_t variant) {
    if (bank.empty() || bank == "default") {
        if (variant > 0) {
            return std::string(name) + ":" + std::to_string(variant);
        }
        return std::string(name);
    }
    return std::string(bank) + "_" + std::string(name) + "_" +
           std::to_string(variant);
}

// Apply kind=0 (StateInit) records to the VM. kind=1/2 records are skipped
// here — see resolve_sample_ids() and patch_sample_ids_in_bytecode().
// Returns # of StateInit records applied, or -1 on malformed input.
inline std::int32_t
apply_state_inits(cedar::VM& vm, const std::uint8_t* buf,
                  std::uint32_t byte_count) {
    using namespace detail;
    if (!buf || byte_count < 8) return -1;
    Reader r{buf, buf + byte_count};

    if (r.u32() != MAGIC_STATE_INITS) return -1;
    if (r.u16() != WIRE_VERSION)      return -1;
    std::uint16_t record_count = r.u16();
    std::int32_t  applied = 0;

    for (std::uint16_t ri = 0; ri < record_count; ++ri) {
        if (!r.need(12)) return -1;
        auto kind        = static_cast<RecordKind>(r.u8());
        r.skip(3);
        std::uint32_t state_id     = r.u32();
        std::uint32_t payload_size = r.u32();
        if (!r.need(payload_size)) return -1;

        const std::uint8_t* payload_start = r.p;
        Reader pr{payload_start, payload_start + payload_size};

        if (kind != RecordKind::StateInit) {
            r.skip(payload_size);
            continue;
        }

        if (!pr.need(4)) return -1;
        auto type = static_cast<StateInitData::Type>(pr.u8());
        pr.skip(3);

        switch (type) {
        case StateInitData::Type::Timeline: {
            std::uint8_t loop_b = pr.u8();
            pr.skip(3);
            float loop_length = pr.f32();
            std::uint32_t num_points = pr.u32();

            auto& state = vm.states().get_or_create<cedar::TimelineState>(state_id);
            state.num_points = std::min(
                num_points,
                static_cast<std::uint32_t>(cedar::TimelineState::MAX_BREAKPOINTS));
            if (state.num_points > 0) {
                pr.read(state.points,
                        state.num_points * sizeof(cedar::TimelineState::Breakpoint));
                if (num_points > state.num_points) {
                    pr.skip((num_points - state.num_points) *
                            sizeof(cedar::TimelineState::Breakpoint));
                }
            }
            state.loop = (loop_b != 0);
            state.loop_length = loop_length;
            ++applied;
            break;
        }
        case StateInitData::Type::SequenceProgram: {
            float cycle_length = pr.f32();
            std::uint8_t is_sample_pattern_b = pr.u8();
            pr.skip(3);
            std::uint32_t total_events  = pr.u32();
            std::uint32_t num_sequences = pr.u32();

            std::vector<cedar::Sequence> seqs(num_sequences);
            std::vector<std::vector<cedar::Event>> events(num_sequences);
            for (std::uint32_t i = 0; i < num_sequences; ++i) {
                float duration = pr.f32();
                auto mode      = static_cast<cedar::SequenceMode>(pr.u8());
                pr.skip(3);
                std::uint32_t nev = pr.u32();
                events[i].resize(nev);
                if (nev > 0) {
                    pr.read(events[i].data(), nev * sizeof(cedar::Event));
                }
                seqs[i].events     = events[i].empty() ? nullptr : events[i].data();
                seqs[i].num_events = nev;
                seqs[i].capacity   = nev;
                seqs[i].duration   = duration;
                seqs[i].step       = 0;
                seqs[i].mode       = mode;
            }
            vm.init_sequence_program_state(state_id, seqs.data(), seqs.size(),
                                           cycle_length,
                                           is_sample_pattern_b != 0,
                                           total_events);
            ++applied;
            break;
        }
        case StateInitData::Type::PolyAlloc: {
            std::uint32_t seq_state_id = pr.u32();
            std::uint8_t  max_voices   = pr.u8();
            std::uint8_t  mode         = pr.u8();
            std::uint8_t  steal        = pr.u8();
            std::uint8_t  prop_count   = pr.u8();
            float         release_s    = pr.f32();
            float prop_defaults[cedar::MAX_PROPS_PER_EVENT] = {};
            pr.read(prop_defaults, sizeof(prop_defaults));
            vm.init_poly_state(state_id, seq_state_id, max_voices, mode, steal,
                               release_s, prop_count, prop_defaults);
            ++applied;
            break;
        }
        case StateInitData::Type::ExtendedParams: {
            std::uint8_t count = pr.u8();
            pr.skip(3);
            float        constants[MAX_EXTENDED_PARAMS] = {};
            std::uint16_t buffer_indices[MAX_EXTENDED_PARAMS] = {};
            pr.read(constants, sizeof(constants));
            pr.read(buffer_indices, sizeof(buffer_indices));
            vm.init_extended_params(state_id, constants, buffer_indices, count);
            ++applied;
            break;
        }
        case StateInitData::Type::SoundfontEvents: {
#ifndef CEDAR_NO_SOUNDFONT
            std::uint32_t seq_state_id = pr.u32();
            std::int32_t  preset_idx   = pr.i32();
            vm.init_soundfont_voice_event_state(state_id, seq_state_id,
                                                preset_idx);
            ++applied;
#else
            pr.skip(8);
#endif
            break;
        }
        case StateInitData::Type::ForeachAlloc: {
            std::uint8_t  allocator_kind = pr.u8();
            pr.skip(3);
            std::uint32_t block_id       = pr.u32();
            std::uint32_t event_src      = pr.u32();
            std::uint16_t max_iterations = pr.u16();
            pr.skip(2);
            std::uint8_t  max_voices     = pr.u8();
            std::uint8_t  mode           = pr.u8();
            std::uint8_t  steal          = pr.u8();
            std::uint8_t  prop_count     = pr.u8();
            float         release_s      = pr.f32();
            float prop_defaults[cedar::MAX_PROPS_PER_EVENT] = {};
            pr.read(prop_defaults, sizeof(prop_defaults));
            vm.init_foreach_state(state_id, allocator_kind, block_id, event_src,
                                  max_iterations, max_voices, mode, steal,
                                  release_s, prop_count, prop_defaults);
            ++applied;
            break;
        }
        case StateInitData::Type::EventTransform:
        case StateInitData::Type::Reorder:
        case StateInitData::Type::Fanout: {
            float cycle_length = pr.f32();
            std::uint8_t is_sample_pattern_b = pr.u8();
            pr.skip(3);
            std::uint32_t total_events = pr.u32();
            vm.init_event_transform_state(state_id, cycle_length,
                                          is_sample_pattern_b != 0,
                                          total_events);
            ++applied;
            break;
        }
        case StateInitData::Type::RateScale: {
            vm.init_event_rate_scale_state(state_id);
            ++applied;
            break;
        }
        }

        if (!pr.ok()) return -1;
        r.skip(payload_size);
    }

    if (!r.ok()) return -1;
    return applied;
}

// Patch kind=1 (SequenceSampleMapping) record IDs into the events of the
// arena-allocated SequenceState's. Must run AFTER apply_state_inits() and
// AFTER the sample bank is populated.
inline std::int32_t
resolve_sample_ids(cedar::VM& vm, const std::uint8_t* buf,
                   std::uint32_t byte_count) {
    using namespace detail;
    if (!buf || byte_count < 8) return -1;
    Reader r{buf, buf + byte_count};

    if (r.u32() != MAGIC_STATE_INITS) return -1;
    if (r.u16() != WIRE_VERSION)      return -1;
    std::uint16_t record_count = r.u16();
    std::int32_t  patched = 0;

    for (std::uint16_t ri = 0; ri < record_count; ++ri) {
        if (!r.need(12)) return -1;
        auto kind        = static_cast<RecordKind>(r.u8());
        r.skip(3);
        std::uint32_t state_id     = r.u32();
        std::uint32_t payload_size = r.u32();
        if (!r.need(payload_size)) return -1;

        if (kind != RecordKind::SequenceSampleMapping) {
            r.skip(payload_size);
            continue;
        }

        Reader pr{r.p, r.p + payload_size};
        std::uint32_t count = pr.u32();
        auto* seq_state = vm.states().get_if<cedar::SequenceState>(state_id);

        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint16_t seq_idx     = pr.u16();
            std::uint16_t event_idx   = pr.u16();
            std::uint8_t  value_slot  = pr.u8();
            std::uint8_t  variant     = pr.u8();
            pr.skip(2);
            std::uint16_t bank_len    = pr.u16();
            std::uint16_t name_len    = pr.u16();
            std::string_view bank_sv  = pr.str(bank_len);
            std::string_view name_sv  = pr.str(name_len);

            if (!seq_state) continue;
            if (seq_idx >= seq_state->num_sequences) continue;
            auto& seq = seq_state->sequences[seq_idx];
            if (event_idx >= seq.num_events) continue;
            if (value_slot >= cedar::MAX_VALUES_PER_EVENT) continue;

            std::string lookup = make_sample_lookup_name(bank_sv, name_sv,
                                                         variant);
            std::uint32_t id = vm.sample_bank().get_sample_id(lookup);
            seq.events[event_idx].values[value_slot] =
                static_cast<float>(id);
            ++patched;
        }

        if (!pr.ok()) return -1;
        r.skip(payload_size);
    }

    if (!r.ok()) return -1;
    return patched;
}

// Apply midiSourcesBuf records to the VM. Must run AFTER load_program.
inline std::int32_t
apply_midi_sources(cedar::VM& vm, const std::uint8_t* buf,
                   std::uint32_t byte_count) {
    using namespace detail;
    if (!buf || byte_count < 8) return -1;
    Reader r{buf, buf + byte_count};

    if (r.u32() != MAGIC_MIDI_SOURCES) return -1;
    if (r.u16() != WIRE_VERSION)       return -1;
    std::uint16_t record_count = r.u16();
    std::int32_t  applied = 0;

    for (std::uint16_t ri = 0; ri < record_count; ++ri) {
        if (!r.need(16)) return -1;
        std::uint32_t state_id       = r.u32();
        auto          kind           = static_cast<cedar::MidiSourceKind>(r.u8());
        std::uint8_t  channel_filter = r.u8();
        std::uint8_t  loop_b         = r.u8();
        auto          tempo_mode     =
            static_cast<cedar::MidiQueueState::TempoMode>(r.u8());
        std::uint16_t name_len       = r.u16();
        r.skip(2);
        std::string_view name_sv     = r.str(name_len);

        // 4-byte alignment matching the packer's tail-pad.
        std::size_t consumed = 4 + 4 + 4 + name_len;
        std::size_t pad      = (4 - (consumed % 4)) % 4;
        r.skip(pad);

        if (!r.ok()) return -1;

        std::string name(name_sv);
        vm.init_midi_queue_state(state_id, kind,
                                 name.empty() ? nullptr : name.c_str(),
                                 channel_filter, loop_b != 0, tempo_mode);
        ++applied;
    }

    if (!r.ok()) return -1;
    return applied;
}

// Patch PUSH_CONST immediates in `bytecode` from kind=2 records in
// stateInitsBuf. Returns # patched, or -1 on malformed input.
inline std::int32_t
patch_sample_ids_in_bytecode(cedar::VM& vm,
                             cedar::Instruction* bytecode,
                             std::uint32_t inst_count,
                             const std::uint8_t* buf,
                             std::uint32_t byte_count) {
    using namespace detail;
    if (!bytecode) return -1;
    if (!buf || byte_count < 8) return -1;
    Reader r{buf, buf + byte_count};

    if (r.u32() != MAGIC_STATE_INITS) return -1;
    if (r.u16() != WIRE_VERSION)      return -1;
    std::uint16_t record_count = r.u16();
    std::int32_t  patched = 0;

    for (std::uint16_t ri = 0; ri < record_count; ++ri) {
        if (!r.need(12)) return -1;
        auto kind        = static_cast<RecordKind>(r.u8());
        r.skip(3);
        r.u32();  // state_id (unused for kind=2)
        std::uint32_t payload_size = r.u32();
        if (!r.need(payload_size)) return -1;

        if (kind != RecordKind::ScalarSampleMapping) {
            r.skip(payload_size);
            continue;
        }

        Reader pr{r.p, r.p + payload_size};
        std::uint32_t count = pr.u32();
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t instruction_index = pr.u32();
            std::uint8_t  variant           = pr.u8();
            pr.skip(3);
            std::uint16_t bank_len = pr.u16();
            std::uint16_t name_len = pr.u16();
            std::string_view bank_sv = pr.str(bank_len);
            std::string_view name_sv = pr.str(name_len);

            if (instruction_index >= inst_count) continue;
            auto& inst = bytecode[instruction_index];
            if (inst.opcode != cedar::Opcode::PUSH_CONST) continue;

            std::string lookup = make_sample_lookup_name(bank_sv, name_sv,
                                                         variant);
            std::uint32_t id = vm.sample_bank().get_sample_id(lookup);
            float as_float = static_cast<float>(id);
            std::memcpy(&inst.state_id, &as_float, sizeof(float));
            ++patched;
        }

        if (!pr.ok()) return -1;
        r.skip(payload_size);
    }

    if (!r.ok()) return -1;
    return patched;
}

} // namespace akkado::state_init_buffer
