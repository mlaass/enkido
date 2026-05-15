#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

// Forward-declare RtMidiIn so this header doesn't pull in rtmidi for every TU
// that only needs the route-table types.
class RtMidiIn;

namespace cedar { class VM; }

namespace nkido {

// One row of the per-device route table. The MIDI callback walks the table
// for each incoming message and calls vm.push_midi_event(state_id, ...) for
// every entry whose channel_filter matches.
struct MidiRoute {
    std::uint32_t state_id = 0;
    std::uint8_t  channel_filter = 0;  // 0 = any, 1..16 = match MIDI channel
};

using MidiRouteTable = std::vector<MidiRoute>;

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
};

}  // namespace nkido
