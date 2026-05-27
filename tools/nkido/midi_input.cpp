#include "midi_input.hpp"

#include "cedar/vm/vm.hpp"

#include <RtMidi.h>

#include <iostream>
#include <utility>

namespace nkido {

MidiInput::MidiInput(cedar::VM& vm) : vm_(vm) {
    try {
        rt_ = std::make_unique<RtMidiIn>();
    } catch (const RtMidiError& e) {
        std::cerr << "warning: RtMidiIn construction failed: "
                  << e.getMessage() << "\n";
        rt_.reset();
    }
    // Start with an empty route table so callbacks fired before
    // set_route_table find nothing to dispatch (no UB on null deref).
    route_table_ = std::make_shared<const MidiRouteTable>();
    cc_route_table_ = std::make_shared<const MidiCcRouteTable>();
}

MidiInput::~MidiInput() {
    close();
}

bool MidiInput::open(const std::string& port_substr) {
    if (!rt_) return false;
    if (is_open_) close();

    match_substr_ = port_substr;

    unsigned int n = 0;
    try {
        n = rt_->getPortCount();
    } catch (const RtMidiError& e) {
        std::cerr << "warning: rtmidi getPortCount failed: "
                  << e.getMessage() << "\n";
        return false;
    }
    if (n == 0) return false;

    int chosen = -1;
    for (unsigned int i = 0; i < n; ++i) {
        std::string name;
        try { name = rt_->getPortName(i); }
        catch (const RtMidiError&) { continue; }
        if (port_substr.empty() || name.find(port_substr) != std::string::npos) {
            chosen = static_cast<int>(i);
            port_name_ = std::move(name);
            break;
        }
    }
    if (chosen < 0) return false;

    try {
        rt_->openPort(static_cast<unsigned int>(chosen),
                      "nkido MIDI input");
        // Ignore sysex/timing/active-sense — we don't process them in v1.
        rt_->ignoreTypes(true, true, true);
        rt_->setCallback(&MidiInput::on_message, this);
    } catch (const RtMidiError& e) {
        std::cerr << "warning: rtmidi openPort failed for '" << port_name_
                  << "': " << e.getMessage() << "\n";
        port_name_.clear();
        return false;
    }

    is_open_ = true;
    return true;
}

void MidiInput::close() {
    if (rt_ && is_open_) {
        try {
            rt_->cancelCallback();
            rt_->closePort();
        } catch (const RtMidiError&) {
            // Closing should not throw in practice; swallow if it does.
        }
    }
    is_open_ = false;
    port_name_.clear();
    set_route_table(std::make_shared<const MidiRouteTable>());
    set_cc_route_table(std::make_shared<const MidiCcRouteTable>());
}

void MidiInput::set_route_table(std::shared_ptr<const MidiRouteTable> table) {
    std::lock_guard<std::mutex> lock(route_mutex_);
    route_table_ = std::move(table);
}

void MidiInput::set_cc_route_table(std::shared_ptr<const MidiCcRouteTable> table) {
    std::lock_guard<std::mutex> lock(route_mutex_);
    cc_route_table_ = std::move(table);
}

void MidiInput::list_ports(std::ostream& out) {
    std::unique_ptr<RtMidiIn> in;
    try {
        in = std::make_unique<RtMidiIn>();
    } catch (const RtMidiError& e) {
        out << "error: rtmidi init failed: " << e.getMessage() << "\n";
        return;
    }

    unsigned int n = 0;
    try { n = in->getPortCount(); }
    catch (const RtMidiError& e) {
        out << "error: rtmidi getPortCount failed: " << e.getMessage() << "\n";
        return;
    }

    out << "MIDI input devices (" << n << "):\n";
    if (n == 0) {
        out << "  (no MIDI input devices found)\n";
        return;
    }
    for (unsigned int i = 0; i < n; ++i) {
        std::string name;
        try { name = in->getPortName(i); }
        catch (const RtMidiError&) { name = "<unknown>"; }
        out << "  " << i << ": " << name << "\n";
    }
}

void MidiInput::on_message(double /*dt*/,
                           std::vector<unsigned char>* msg,
                           void* userdata) {
    if (!msg || msg->empty()) return;
    auto* self = static_cast<MidiInput*>(userdata);

    const unsigned char status = (*msg)[0];
    // Realtime messages (0xF8..0xFF) and channel-less system messages
    // (0xF0..0xF7) carry no channel; we drop them — Phase 1 cedar discards
    // them anyway, so saving a push_midi_event is a small win.
    if (status >= 0xF0) return;

    const std::uint8_t d1 = msg->size() > 1 ? (*msg)[1] : 0;
    const std::uint8_t d2 = msg->size() > 2 ? (*msg)[2] : 0;
    const std::uint8_t channel = static_cast<std::uint8_t>((status & 0x0Fu) + 1u);

    // Snapshot both route tables under one lock-and-release, then dispatch
    // outside the critical section. shared_ptr keeps each table alive even
    // if the main thread swaps in a new one mid-callback.
    std::shared_ptr<const MidiRouteTable> tbl;
    std::shared_ptr<const MidiCcRouteTable> cc_tbl;
    {
        std::lock_guard<std::mutex> lock(self->route_mutex_);
        tbl = self->route_table_;
        cc_tbl = self->cc_route_table_;
    }

    // CC / pitch-bend / aftertouch land in the EnvMap via vm.set_param
    // (PRD prd-midi-input §4.8). Independent of the note ring below.
    if (cc_tbl && !cc_tbl->empty()) {
        dispatch_midi_cc(self->vm_, *cc_tbl, status, d1, d2, channel);
    }

    if (!tbl) return;
    for (const auto& route : *tbl) {
        if (route.channel_filter != 0 && route.channel_filter != channel) continue;
        self->vm_.push_midi_event(route.state_id, status, d1, d2);
    }
}

}  // namespace nkido
