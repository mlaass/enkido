#pragma once

#include "cedar/vm/vm.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

// Forward-declare RtMidiIn so this header doesn't pull in rtmidi for every TU
// that only needs the route-table types.
class RtMidiIn;

namespace nkido {

// One row of the per-device route table. The MIDI callback walks the table
// for each incoming message and calls vm.push_midi_event(state_id, ...) for
// every entry whose channel_filter matches.
struct MidiRoute {
    std::uint32_t state_id = 0;
    std::uint8_t  channel_filter = 0;  // 0 = any, 1..16 = match MIDI channel
};

using MidiRouteTable = std::vector<MidiRoute>;

// One row of the per-device CC-route table (PRD prd-midi-input §4.8). The MIDI
// callback walks the table for each incoming CC / pitch-bend / aftertouch
// message and calls vm.set_param(param_name, value, slew_ms) for every entry
// whose channel_filter + cc_num matches.
//
// cc_num sentinels:
//   0..127 — MIDI CC number (status 0xBn, d1 == cc_num)
//   -1     — pitch-bend (status 0xEn, 14-bit value = (d2<<7)|d1)
//   -2     — channel aftertouch (status 0xDn, d1 = pressure)
struct MidiCcRoute {
    std::string  param_name;
    std::int16_t cc_num         = 0;
    std::uint8_t channel_filter = 0;
    float        scale          = 1.0f;  // max - min
    float        bias           = 0.0f;  // min
    float        slew_ms        = 5.0f;
};

using MidiCcRouteTable = std::vector<MidiCcRoute>;

// Dispatch one parsed MIDI message into a CC-route table. Exposed at the
// header level (rather than as a private member of MidiInput) so it can be
// driven from unit tests without spinning up RtMidi.
//
// Convention: `channel` is 1-indexed (1..16) — same as MidiRoute::channel_filter.
// Caller is responsible for filtering out status bytes outside 0xB0..0xEF if
// it wants — this function silently no-ops for non-CC/PB/AT messages.
inline void dispatch_midi_cc(cedar::VM& vm,
                             const MidiCcRouteTable& table,
                             std::uint8_t status,
                             std::uint8_t d1,
                             std::uint8_t d2,
                             std::uint8_t channel) {
    const std::uint8_t base = static_cast<std::uint8_t>(status & 0xF0u);
    const bool is_cc = (base == 0xB0u);
    const bool is_pb = (base == 0xE0u);
    const bool is_at = (base == 0xD0u);
    if (!is_cc && !is_pb && !is_at) return;

    for (const auto& r : table) {
        if (r.channel_filter != 0 && r.channel_filter != channel) continue;

        float v = 0.0f;
        if (is_cc) {
            if (r.cc_num < 0 || r.cc_num > 127) continue;
            if (d1 != static_cast<std::uint8_t>(r.cc_num)) continue;
            v = (static_cast<float>(d2) / 127.0f) * r.scale + r.bias;
        } else if (is_pb) {
            if (r.cc_num != -1) continue;
            // 14-bit unsigned: (MSB << 7) | LSB → 0..16383, centered at 8192.
            const int v14 = (static_cast<int>(d2) << 7) | static_cast<int>(d1);
            const float n = (static_cast<float>(v14) / 16383.0f) * 2.0f - 1.0f;
            v = n * (r.scale * 0.5f) + (r.bias + r.scale * 0.5f);
        } else {  // is_at
            if (r.cc_num != -2) continue;
            v = (static_cast<float>(d1) / 127.0f) * r.scale + r.bias;
        }

        vm.set_param(r.param_name.c_str(), v, r.slew_ms);
    }
}

// One physical MIDI input port. Owns one RtMidiIn instance and a
// mutex-guarded route table swappable on every bytecode reload.
//
// Thread model:
//   * Constructor / open / close / set_route_table run on the main (CLI)
//     thread.
//   * on_message runs on the OS-owned MIDI callback thread (per RtMidi
//     backend: ALSA sequencer thread, CoreMIDI Mach port thread, WinMM
//     callback). It loads a snapshot of the route table under the mutex,
//     then dispatches each matching event into vm.push_midi_event — which
//     is itself thread-safe (SPSC ring with release/acquire pairing).
//
// Per docs/prd-midi-input.md §4.13: one MidiInput per physical device, one
// route table per device. Multiple state_ids may share a device with
// different channel filters.
class MidiInput {
public:
    explicit MidiInput(cedar::VM& vm);
    ~MidiInput();

    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    // Open the first available input port whose name contains
    // `port_substr` (case-sensitive). An empty `port_substr` opens the
    // first port (port 0). Returns false if no port matches or no ports
    // exist; callers warn-and-continue per the audio-input pattern.
    bool open(const std::string& port_substr);

    // Close the underlying RtMidiIn port and replace the route table with
    // an empty one so any in-flight callback becomes a no-op.
    void close();

    [[nodiscard]] bool is_open() const { return is_open_; }
    [[nodiscard]] const std::string& port_name() const { return port_name_; }
    [[nodiscard]] const std::string& match_substr() const { return match_substr_; }

    // Atomically replace the route table. Subsequent callbacks see the new
    // table; in-flight callbacks finish with their already-loaded snapshot.
    void set_route_table(std::shared_ptr<const MidiRouteTable> table);

    // Atomically replace the CC-route table (PRD prd-midi-input §4.8).
    // Independent of the note route table; same mutex guards both swaps.
    void set_cc_route_table(std::shared_ptr<const MidiCcRouteTable> table);

    // Print every available MIDI input port (port_index: name) to `out`.
    // Self-contained: instantiates a temporary RtMidiIn for enumeration.
    static void list_ports(std::ostream& out);

private:
    // RtMidi C-style callback signature.
    static void on_message(double dt,
                           std::vector<unsigned char>* msg,
                           void* userdata);

    cedar::VM& vm_;
    std::unique_ptr<RtMidiIn> rt_;
    std::string port_name_;
    std::string match_substr_;  // remembers what apply_midi_route_plan keyed on
    bool is_open_ = false;

    mutable std::mutex route_mutex_;
    std::shared_ptr<const MidiRouteTable> route_table_;
    std::shared_ptr<const MidiCcRouteTable> cc_route_table_;
};

}  // namespace nkido
