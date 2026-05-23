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
