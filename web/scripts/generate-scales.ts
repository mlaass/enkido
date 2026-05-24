/**
 * Generate akkado/stdlib/scales.ak — the scale-quantization catalog.
 *
 * Emits two dispatcher fns matching the `osc(kind, …)` style used elsewhere
 * in the stdlib (see akkado/include/akkado/stdlib.hpp:16-38):
 *
 *   fn scale_intervals(name) -> match(name) {
 *       "minor": [0, 2, 3, 5, 7, 8, 10]
 *       …
 *       _: [0, 2, 4, 5, 7, 9, 11]  // default = major
 *   }
 *
 *   fn scale_length(name) -> match(name) {
 *       "minor": 7
 *       …
 *       _: 7
 *   }
 *
 * `scale_length` is shipped alongside `scale_intervals` because the akkado
 * analyzer's `len()` does not currently trace through fn returns
 * (codegen_arrays.cpp:957). The twin-dispatcher form is also cheap — at
 * compile time only one branch survives constant folding when `name` is a
 * string literal, so this costs zero runtime.
 *
 * Constraint (shared by every match-based stdlib dispatcher: osc, glide,
 * …): the scrutinee must be a string literal at the call site. With a
 * runtime-valued `name` (a fn parameter or let-binding), codegen
 * materializes every branch's result and exhausts the buffer pool with
 * O(catalog × scale_length) buffers. Commit F's `key`/`scale` route
 * around this by being the outermost match themselves.
 *
 * Scale data is taken from tonal.js (https://github.com/tonaljs/tonal,
 * MIT license) — see the SCALES table below. We inline a curated subset
 * of the most-used scales rather than pulling tonal.js as a build-time
 * dependency.
 *
 * Usage: bun run scripts/generate-scales.ts
 */

import { writeFileSync } from "fs";
import { resolve } from "path";

const ROOT_DIR = resolve(import.meta.dir, "../..");
const OUTPUT_FILE = resolve(ROOT_DIR, "akkado/stdlib/scales.ak");

// ----------------------------------------------------------------------------
// Interval-quality → semitone conversion
// ----------------------------------------------------------------------------
//
// Interval quality notation (1P, 2M, 3m, 4A, 5d, …): position + quality.
// Quality letters: P = perfect, M = major, m = minor, A = augmented,
// d = diminished. Augmented = perfect+1 OR major+1; diminished = perfect-1
// OR minor-1. Reference: https://en.wikipedia.org/wiki/Interval_(music)

const PERFECT: Record<number, number> = { 1: 0, 4: 5, 5: 7, 8: 12 };
const MAJOR: Record<number, number> = { 2: 2, 3: 4, 6: 9, 7: 11 };
const MINOR: Record<number, number> = { 2: 1, 3: 3, 6: 8, 7: 10 };

function intervalToSemitones(token: string): number {
  const match = token.match(/^(\d+)([PMmAd])$/);
  if (!match) {
    throw new Error(`invalid interval token: '${token}'`);
  }
  const pos = parseInt(match[1], 10);
  const qual = match[2];
  if (qual === "P") {
    if (PERFECT[pos] === undefined) {
      throw new Error(`no perfect ${pos}th`);
    }
    return PERFECT[pos];
  }
  if (qual === "M") {
    if (MAJOR[pos] === undefined) {
      throw new Error(`no major ${pos}th`);
    }
    return MAJOR[pos];
  }
  if (qual === "m") {
    if (MINOR[pos] === undefined) {
      throw new Error(`no minor ${pos}th`);
    }
    return MINOR[pos];
  }
  if (qual === "A") {
    if (PERFECT[pos] !== undefined) return PERFECT[pos] + 1;
    if (MAJOR[pos] !== undefined) return MAJOR[pos] + 1;
    throw new Error(`no augmented ${pos}th`);
  }
  if (qual === "d") {
    if (PERFECT[pos] !== undefined) return PERFECT[pos] - 1;
    if (MINOR[pos] !== undefined) return MINOR[pos] - 1;
    throw new Error(`no diminished ${pos}th`);
  }
  throw new Error(`unknown quality '${qual}' in '${token}'`);
}

function intervalsToSemitones(intervals: string): number[] {
  return intervals.trim().split(/\s+/).map(intervalToSemitones);
}

// ----------------------------------------------------------------------------
// Scale catalog (curated tonal.js subset).
// ----------------------------------------------------------------------------
//
// Each entry: { name, intervals_quality, aliases? }. The catalog is
// ordered by family (modes of major first, melodic/harmonic minor modes,
// pentatonic + blues, symmetric, exotic) and the `_:` default branch
// matches the major scale.

interface ScaleEntry {
  name: string;
  intervals: string;
  aliases?: string[];
}

const SCALES: ScaleEntry[] = [
  // Diatonic modes of the major scale
  { name: "major", intervals: "1P 2M 3M 4P 5P 6M 7M", aliases: ["ionian"] },
  { name: "minor", intervals: "1P 2M 3m 4P 5P 6m 7m", aliases: ["aeolian", "natural_minor"] },
  { name: "dorian", intervals: "1P 2M 3m 4P 5P 6M 7m" },
  { name: "phrygian", intervals: "1P 2m 3m 4P 5P 6m 7m" },
  { name: "lydian", intervals: "1P 2M 3M 4A 5P 6M 7M" },
  { name: "mixolydian", intervals: "1P 2M 3M 4P 5P 6M 7m" },
  { name: "locrian", intervals: "1P 2m 3m 4P 5d 6m 7m" },

  // Melodic / harmonic minor and their main modes
  { name: "harmonic_minor", intervals: "1P 2M 3m 4P 5P 6m 7M" },
  { name: "melodic_minor", intervals: "1P 2M 3m 4P 5P 6M 7M" },
  { name: "phrygian_dominant", intervals: "1P 2m 3M 4P 5P 6m 7m" },
  { name: "lydian_augmented", intervals: "1P 2M 3M 4A 5A 6M 7M" },
  { name: "lydian_dominant", intervals: "1P 2M 3M 4A 5P 6M 7m" },
  { name: "mixolydian_b6", intervals: "1P 2M 3M 4P 5P 6m 7m" },
  { name: "locrian_natural2", intervals: "1P 2M 3m 4P 5d 6m 7m" },
  { name: "altered", intervals: "1P 2m 3m 3M 5d 6m 7m" },

  // Pentatonic + blues
  { name: "major_pentatonic", intervals: "1P 2M 3M 5P 6M" },
  { name: "minor_pentatonic", intervals: "1P 3m 4P 5P 7m" },
  { name: "blues", intervals: "1P 3m 4P 4A 5P 7m" },
  { name: "major_blues", intervals: "1P 2M 3m 3M 5P 6M" },

  // Symmetric
  { name: "chromatic", intervals: "1P 2m 2M 3m 3M 4P 4A 5P 6m 6M 7m 7M" },
  { name: "whole_tone", intervals: "1P 2M 3M 4A 5A 7m" },
  { name: "diminished", intervals: "1P 2M 3m 4P 5d 6m 6M 7M" },
  { name: "augmented", intervals: "1P 3m 3M 5P 5A 7M" },

  // Exotic / world
  { name: "hungarian_minor", intervals: "1P 2M 3m 4A 5P 6m 7M" },
  { name: "double_harmonic", intervals: "1P 2m 3M 4P 5P 6m 7M", aliases: ["byzantine"] },
  { name: "neapolitan_minor", intervals: "1P 2m 3m 4P 5P 6m 7M" },
  { name: "neapolitan_major", intervals: "1P 2m 3m 4P 5P 6M 7M" },
  { name: "hirajoshi", intervals: "1P 2M 3m 5P 6m" },
  { name: "in_sen", intervals: "1P 2m 4P 5P 7m" },
  { name: "iwato", intervals: "1P 2m 4P 5d 7m" },
  { name: "yo", intervals: "1P 2M 4P 5P 6M" },
  { name: "ryukyu", intervals: "1P 3M 4P 5P 7M" },

  // Bebop family
  { name: "bebop_major", intervals: "1P 2M 3M 4P 5P 5A 6M 7M" },
  { name: "bebop_minor", intervals: "1P 2M 3m 3M 4P 5P 6M 7m" },
  { name: "bebop_dominant", intervals: "1P 2M 3M 4P 5P 6M 7m 7M" },
];

const DEFAULT_INTERVALS = intervalsToSemitones(SCALES[0].intervals);
const DEFAULT_LENGTH = DEFAULT_INTERVALS.length;

// ----------------------------------------------------------------------------
// Emission
// ----------------------------------------------------------------------------

function formatArray(xs: number[]): string {
  return "[" + xs.join(", ") + "]";
}

function emitScalesAk(): string {
  const lines: string[] = [];
  lines.push("// Akkado stdlib — scale catalog (auto-generated).");
  lines.push("//");
  lines.push("// AUTO-GENERATED FILE — DO NOT EDIT BY HAND.");
  lines.push("//");
  lines.push("// Generated from web/scripts/generate-scales.ts. To regenerate:");
  lines.push("//     cd web && bun run scripts/generate-scales.ts");
  lines.push("//");
  lines.push("// Source: curated subset of tonal.js scale-type data");
  lines.push("// (https://github.com/tonaljs/tonal, MIT license). Interval-quality");
  lines.push("// strings ('1P 2M 3m …') are converted to semitone offsets at");
  lines.push("// generation time.");
  lines.push("//");
  lines.push("// Phase 5 of docs/prd-runtime-event-transforms.md ships this catalog.");
  lines.push("// `scale_intervals(name)` returns the chromatic offsets of a scale's");
  lines.push("// pitches relative to its tonic; `scale_length(name)` returns the");
  lines.push("// pitch count. Both are pure dispatchers in the same shape as");
  lines.push("// `osc(kind, …)` (akkado/include/akkado/stdlib.hpp) — when the");
  lines.push("// caller passes a string literal, constant folding eliminates all");
  lines.push("// but one branch at compile time.");
  lines.push("//");
  lines.push("// `scale_length` is shipped alongside `scale_intervals` because");
  lines.push("// akkado's `len()` does not trace through fn returns; see");
  lines.push("// akkado/src/codegen_arrays.cpp:957. The twin-dispatcher form lets");
  lines.push("// downstream `degree_to_note` compute `step = d - oct * k` without");
  lines.push("// `len()`.");
  lines.push("//");
  lines.push("// Constraint: call these dispatchers with a STRING LITERAL only.");
  lines.push("// With a runtime-valued `name` (a fn parameter or let-binding),");
  lines.push("// match codegen can't fold to one branch and buffers exhaust at");
  lines.push("// O(catalog_size × max_scale_length). `key` / `scale` in Commit F");
  lines.push("// route around this by being the outer match themselves.");
  lines.push("");

  // Build map name → semitones with alias expansion.
  const expanded: Array<{ key: string; semis: number[] }> = [];
  for (const entry of SCALES) {
    const semis = intervalsToSemitones(entry.intervals);
    expanded.push({ key: entry.name, semis });
    if (entry.aliases) {
      for (const alias of entry.aliases) {
        expanded.push({ key: alias, semis });
      }
    }
  }

  // Emit scale_intervals(name) -> match(name) { … }
  lines.push("fn scale_intervals(name) -> match(name) {");
  for (const { key, semis } of expanded) {
    lines.push(`    "${key}": ${formatArray(semis)}`);
  }
  lines.push(`    _: ${formatArray(DEFAULT_INTERVALS)}`);
  lines.push("}");
  lines.push("");

  // Emit scale_length(name) -> match(name) { … }
  lines.push("fn scale_length(name) -> match(name) {");
  for (const { key, semis } of expanded) {
    lines.push(`    "${key}": ${semis.length}`);
  }
  lines.push(`    _: ${DEFAULT_LENGTH}`);
  lines.push("}");
  lines.push("");

  return lines.join("\n");
}

// ----------------------------------------------------------------------------
// Unit spot-check — compare ~10 well-known scales to known correct values.
// ----------------------------------------------------------------------------

const EXPECTED: Record<string, number[]> = {
  major: [0, 2, 4, 5, 7, 9, 11],
  minor: [0, 2, 3, 5, 7, 8, 10],
  dorian: [0, 2, 3, 5, 7, 9, 10],
  phrygian: [0, 1, 3, 5, 7, 8, 10],
  lydian: [0, 2, 4, 6, 7, 9, 11],
  mixolydian: [0, 2, 4, 5, 7, 9, 10],
  locrian: [0, 1, 3, 5, 6, 8, 10],
  harmonic_minor: [0, 2, 3, 5, 7, 8, 11],
  melodic_minor: [0, 2, 3, 5, 7, 9, 11],
  major_pentatonic: [0, 2, 4, 7, 9],
  minor_pentatonic: [0, 3, 5, 7, 10],
  whole_tone: [0, 2, 4, 6, 8, 10],
  chromatic: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11],
};

function runSpotChecks(): void {
  let failed = 0;
  for (const [name, expected] of Object.entries(EXPECTED)) {
    const entry = SCALES.find((s) => s.name === name);
    if (!entry) {
      console.error(`  ✗ ${name}: not in SCALES table`);
      failed++;
      continue;
    }
    const actual = intervalsToSemitones(entry.intervals);
    const match = actual.length === expected.length && actual.every((v, i) => v === expected[i]);
    if (!match) {
      console.error(`  ✗ ${name}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
      failed++;
    } else {
      console.log(`  ✓ ${name}: ${JSON.stringify(actual)}`);
    }
  }
  if (failed > 0) {
    console.error(`\n${failed} spot-check(s) failed`);
    process.exit(1);
  }
  console.log(`\n${Object.keys(EXPECTED).length} spot-checks passed`);
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------

console.log("Running spot-checks...");
runSpotChecks();

console.log("\nGenerating scales.ak...");
const content = emitScalesAk();
writeFileSync(OUTPUT_FILE, content);
console.log(`  wrote ${OUTPUT_FILE} (${content.split("\n").length} lines)`);
