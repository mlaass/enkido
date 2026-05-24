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

fn transpose(events: evs, n)  -> { event_map(events, (e) -> {note: e.note + n}) }
fn velocity(events: evs, v)   -> { event_map(events, (e) -> {vel:  e.vel * v}) }
fn dur(events: evs, d)        -> { event_map(events, (e) -> {dur:  e.dur * d}) }
fn bend(events: evs, b)       -> { event_map(events, (e) -> {bend: b}) }
fn aftertouch(events: evs, a) -> { event_map(events, (e) -> {at:   a}) }

// ----------------------------------------------------------------------------
// Time-shift modifiers — overlay e.time with a phase-wrapped offset.
// ----------------------------------------------------------------------------
// The EVENT_MAP closure runtime resorts the output by time when the TIME
// write-mask bit is set (event_transforms.hpp:186-188), so events that wrap
// past the cycle boundary land in the correct block position. `fmod` keeps
// the result inside [0, 1); for negative results we add 1.0 first because C's
// fmod preserves the sign of the dividend (fmod(-0.25, 1.0) = -0.25).

fn early(events: evs, t) -> { event_map(events, (e) -> {time: fmod(e.time - t + 1.0, 1.0)}) }
fn late(events: evs, t)  -> { event_map(events, (e) -> {time: fmod(e.time + t, 1.0)}) }

// ----------------------------------------------------------------------------
// Swing — delay the off-beats of an N-slice grid by `amount * slice_width / 2`.
// ----------------------------------------------------------------------------
// Matches the pre-Phase-2b `apply_swing` math in codegen_patterns.cpp: split
// the cycle into `grid` equal slices; for every event whose position within
// its slice is past the halfway mark, add a forward time shift.
//
// API change from pre-Phase-2b: default `grid` is now 8 (PRD §11 OQ-10),
// up from the legacy default of 4. Pass `grid: 4` explicitly to restore the
// old grid resolution.

fn swing_offset(time, amount, grid) -> {
    select(gte(time * grid - floor(time * grid), 0.5), amount * 0.5 / grid, 0)
}

fn swingBy(events: evs, amount, grid = 8) -> {
    event_map(events, (e) -> {time: fmod(e.time + swing_offset(e.time, amount, grid), 1.0)})
}

fn swing(events: evs, grid = 8) -> { swingBy(events, 1.0 / 3.0, grid) }

// ----------------------------------------------------------------------------
// Chord-array transforms — Phase 5 Commits E + G (chord WRITE substrate).
// ----------------------------------------------------------------------------
// `voice` synthesizes a chord per event by mapping a literal interval array
// over `e.note` — `voice([0, 3, 7])` builds a minor triad on every input note.
// `invert` reflects each chord voice around an axis MIDI pitch — useful for
// chord inversion / mirroring effects.
//
// Both rely on the closure-side chord-array WRITE substrate (`{notes: <arr>}`)
// shipped in Commit E. The runtime overlay derives `e.values[]` (Hz) from the
// new `e.notes[]` (MIDI) via mtof so downstream voicing reads stay consistent.

fn voice(events: evs, intervals) -> {
    event_map(events, (e) -> {notes: map(intervals, (i) -> e.note + i)})
}

fn invert(events: evs, axis) -> {
    event_map(events, (e) -> {notes: map(e.notes, (n) -> 2 * axis - n)})
}
