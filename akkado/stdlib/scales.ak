// Akkado stdlib — scale catalog (auto-generated).
//
// AUTO-GENERATED FILE — DO NOT EDIT BY HAND.
//
// Generated from web/scripts/generate-scales.ts. To regenerate:
//     cd web && bun run scripts/generate-scales.ts
//
// Source: curated subset of tonal.js scale-type data
// (https://github.com/tonaljs/tonal, MIT license). Interval-quality
// strings ('1P 2M 3m …') are converted to semitone offsets at
// generation time.
//
// Phase 5 of docs/prd-runtime-event-transforms.md ships this catalog.
// `scale_intervals(name)` returns the chromatic offsets of a scale's
// pitches relative to its tonic; `scale_length(name)` returns the
// pitch count. Both are pure dispatchers in the same shape as
// `osc(kind, …)` (akkado/include/akkado/stdlib.hpp) — when the
// caller passes a string literal, constant folding eliminates all
// but one branch at compile time.
//
// `scale_length` is shipped alongside `scale_intervals` because
// akkado's `len()` does not trace through fn returns; see
// akkado/src/codegen_arrays.cpp:957. The twin-dispatcher form lets
// downstream `degree_to_note` compute `step = d - oct * k` without
// `len()`.
//
// Constraint: call these dispatchers with a STRING LITERAL only.
// With a runtime-valued `name` (a fn parameter or let-binding),
// match codegen can't fold to one branch and buffers exhaust at
// O(catalog_size × max_scale_length). `key` / `scale` in Commit F
// route around this by being the outer match themselves.

fn scale_intervals(name) -> match(name) {
    "major": [0, 2, 4, 5, 7, 9, 11]
    "ionian": [0, 2, 4, 5, 7, 9, 11]
    "minor": [0, 2, 3, 5, 7, 8, 10]
    "aeolian": [0, 2, 3, 5, 7, 8, 10]
    "natural_minor": [0, 2, 3, 5, 7, 8, 10]
    "dorian": [0, 2, 3, 5, 7, 9, 10]
    "phrygian": [0, 1, 3, 5, 7, 8, 10]
    "lydian": [0, 2, 4, 6, 7, 9, 11]
    "mixolydian": [0, 2, 4, 5, 7, 9, 10]
    "locrian": [0, 1, 3, 5, 6, 8, 10]
    "harmonic_minor": [0, 2, 3, 5, 7, 8, 11]
    "melodic_minor": [0, 2, 3, 5, 7, 9, 11]
    "phrygian_dominant": [0, 1, 4, 5, 7, 8, 10]
    "lydian_augmented": [0, 2, 4, 6, 8, 9, 11]
    "lydian_dominant": [0, 2, 4, 6, 7, 9, 10]
    "mixolydian_b6": [0, 2, 4, 5, 7, 8, 10]
    "locrian_natural2": [0, 2, 3, 5, 6, 8, 10]
    "altered": [0, 1, 3, 4, 6, 8, 10]
    "major_pentatonic": [0, 2, 4, 7, 9]
    "minor_pentatonic": [0, 3, 5, 7, 10]
    "blues": [0, 3, 5, 6, 7, 10]
    "major_blues": [0, 2, 3, 4, 7, 9]
    "chromatic": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
    "whole_tone": [0, 2, 4, 6, 8, 10]
    "diminished": [0, 2, 3, 5, 6, 8, 9, 11]
    "augmented": [0, 3, 4, 7, 8, 11]
    "hungarian_minor": [0, 2, 3, 6, 7, 8, 11]
    "double_harmonic": [0, 1, 4, 5, 7, 8, 11]
    "byzantine": [0, 1, 4, 5, 7, 8, 11]
    "neapolitan_minor": [0, 1, 3, 5, 7, 8, 11]
    "neapolitan_major": [0, 1, 3, 5, 7, 9, 11]
    "hirajoshi": [0, 2, 3, 7, 8]
    "in_sen": [0, 1, 5, 7, 10]
    "iwato": [0, 1, 5, 6, 10]
    "yo": [0, 2, 5, 7, 9]
    "ryukyu": [0, 4, 5, 7, 11]
    "bebop_major": [0, 2, 4, 5, 7, 8, 9, 11]
    "bebop_minor": [0, 2, 3, 4, 5, 7, 9, 10]
    "bebop_dominant": [0, 2, 4, 5, 7, 9, 10, 11]
    _: [0, 2, 4, 5, 7, 9, 11]
}

fn scale_length(name) -> match(name) {
    "major": 7
    "ionian": 7
    "minor": 7
    "aeolian": 7
    "natural_minor": 7
    "dorian": 7
    "phrygian": 7
    "lydian": 7
    "mixolydian": 7
    "locrian": 7
    "harmonic_minor": 7
    "melodic_minor": 7
    "phrygian_dominant": 7
    "lydian_augmented": 7
    "lydian_dominant": 7
    "mixolydian_b6": 7
    "locrian_natural2": 7
    "altered": 7
    "major_pentatonic": 5
    "minor_pentatonic": 5
    "blues": 6
    "major_blues": 6
    "chromatic": 12
    "whole_tone": 6
    "diminished": 8
    "augmented": 6
    "hungarian_minor": 7
    "double_harmonic": 7
    "byzantine": 7
    "neapolitan_minor": 7
    "neapolitan_major": 7
    "hirajoshi": 5
    "in_sen": 5
    "iwato": 5
    "yo": 5
    "ryukyu": 5
    "bebop_major": 8
    "bebop_minor": 8
    "bebop_dominant": 8
    _: 7
}
