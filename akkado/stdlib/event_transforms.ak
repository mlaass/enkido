// Akkado stdlib — event-stream transforms
//
// One-line modifier definitions on top of the runtime closure-EVENT_MAP
// infrastructure shipped in prd-runtime-event-transforms Phase 2a.
//
// Each `fn` here replaces a former C++ handler in
// `akkado/src/codegen_patterns.cpp`. User code can shadow these by defining
// their own `fn transpose`, `fn velocity`, etc.
//
// Migration tracker: docs/prd-runtime-event-transforms.md §9 Phase 2b.

// ----------------------------------------------------------------------------
// Property modifiers — shallow-overlay rewrites on a single event field.
// ----------------------------------------------------------------------------
// NOTE writes are coupled: the runtime opcode rewrites `midi_note` AND every
// chord voice's `notes[]`/`values[]`, so `transpose(7)` lifts an entire chord
// stack. VEL / DUR / BEND / AT are scalar-per-event fields with no chord-voice
// fanout.

fn transpose(events: stream, n)  -> { event_map(events, (e) -> {note: e.note + n}) }
fn velocity(events: stream, v)   -> { event_map(events, (e) -> {vel:  e.vel * v}) }
fn dur(events: stream, d)        -> { event_map(events, (e) -> {dur:  e.dur * d}) }
fn bend(events: stream, b)       -> { event_map(events, (e) -> {bend: b}) }
fn aftertouch(events: stream, a) -> { event_map(events, (e) -> {at:   a}) }

// ----------------------------------------------------------------------------
// Time-shift modifiers — overlay e.time with a phase-wrapped offset.
// ----------------------------------------------------------------------------
// The EVENT_MAP closure runtime resorts the output by time when the TIME
// write-mask bit is set (event_transforms.hpp:186-188), so events that wrap
// past the cycle boundary land in the correct block position. `fmod` keeps
// the result inside [0, 1); for negative results we add 1.0 first because C's
// fmod preserves the sign of the dividend (fmod(-0.25, 1.0) = -0.25).

fn early(events: stream, t) -> { event_map(events, (e) -> {time: fmod(e.time - t + 1.0, 1.0)}) }
fn late(events: stream, t)  -> { event_map(events, (e) -> {time: fmod(e.time + t, 1.0)}) }
