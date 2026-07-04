/**
 * Generate akkado/stdlib/scale_quantize.ak — the scale-quantization
 * stdlib (`fn key` + `fn scale`).
 *
 * Per the match-dispatcher literal-only constraint (see scales.ak header),
 * `key` and `scale` cannot internally delegate to a runtime-valued helper
 * like `scale_intervals(name)`; they MUST be the outer match themselves.
 * Each branch inlines the literal interval array and the literal root
 * MIDI number.
 *
 * `key("root:scale")` — octave-agnostic 12-TET quantization. For each
 * single-note event, look up the integer-semitone delta from a 12-entry
 * per-scale table (precomputed at generation time as the nearest pitch
 * class in P with tie → lower MIDI, per docs/prd-scale-quantize.md §4.5
 * + §11.4).
 *
 * `scale("rootN:scale")` — degree mapping. For each event, treat its
 * note as an integer degree `d`; emit MIDI = root_midi + 12·floor(d/k)
 * + intervals[d mod k]. The root_midi is computed at generation time
 * from (root_pc, octave). Per PRD §4.4 + §4.5.
 *
 * Catalog scope: 12 root pitch classes × 39 scale types per dispatcher.
 * `key` ignores octave (so 12 × 39 = 468 branches). `scale` defaults
 * `root_octave` to 3 when the name lacks an octave digit (so we emit
 * both "root:scale" and "rootN:scale" with N in 0..9 — 12 × 10 × 39 =
 * 4680 branches would explode). Compromise: emit just the octave-less
 * form ("root:scale") with root_octave=3, plus one octave-typed variant
 * per known root for octaves 0..9. To keep size manageable, just emit
 * "root:scale" (default octave 3) and "rootN:scale" for N in 1..6.
 *
 * Constraint: STRING LITERAL only at the call site. With a runtime-valued
 * name the match can't fold and the buffer pool exhausts.
 *
 * Usage: bun run scripts/generate-scale-quantize.ts
 */

import { writeFileSync } from "fs";
import { resolve } from "path";

const ROOT_DIR = resolve(import.meta.dir, "../..");
const OUTPUT_FILE = resolve(ROOT_DIR, "akkado/stdlib/scale_quantize.ak");

// ----------------------------------------------------------------------------
// Interval-quality conversion — duplicated from generate-scales.ts.
// Kept in sync via the SCALES array below; ideally the two scripts share a
// module, but for now duplication is simpler than a shared module.
// ----------------------------------------------------------------------------

const PERFECT: Record<number, number> = { 1: 0, 4: 5, 5: 7, 8: 12 };
const MAJOR: Record<number, number> = { 2: 2, 3: 4, 6: 9, 7: 11 };
const MINOR: Record<number, number> = { 2: 1, 3: 3, 6: 8, 7: 10 };

function intervalToSemitones(token: string): number {
  const m = token.match(/^(\d+)([PMmAd])$/);
  if (!m) throw new Error(`invalid interval token: '${token}'`);
  const pos = parseInt(m[1], 10);
  const q = m[2];
  if (q === "P") return PERFECT[pos];
  if (q === "M") return MAJOR[pos];
  if (q === "m") return MINOR[pos];
  if (q === "A") return (PERFECT[pos] ?? MAJOR[pos]) + 1;
  if (q === "d") return (PERFECT[pos] ?? MINOR[pos]) - 1;
  throw new Error(`unknown quality '${q}'`);
}

function intervalsToSemitones(s: string): number[] {
  return s.trim().split(/\s+/).map(intervalToSemitones);
}

// ----------------------------------------------------------------------------
// Scale catalog (duplicated subset of generate-scales.ts).
// ----------------------------------------------------------------------------

interface ScaleEntry {
  name: string;
  intervals: string;
  aliases?: string[];
}

// Curated catalog — kept SMALL because the stdlib is re-parsed on every
// `akkado::compile()` invocation, and the fuzz test suite hits compile in
// the hundreds-of-thousands. Each branch costs a parser+typecheck pass at
// test time. The 8 ships-by-default scales below cover the practical
// live-coding palette (modes + pentatonics + chromatic). Users who want
// `phrygian_dominant`, `bebop_*`, `iwato`, etc. should reach for the v2
// user-defined-scale API (PRD §4.6) — extending the literal catalog has a
// real test-time cost.
const SCALES: ScaleEntry[] = [
  { name: "major", intervals: "1P 2M 3M 4P 5P 6M 7M", aliases: ["ionian"] },
  { name: "minor", intervals: "1P 2M 3m 4P 5P 6m 7m", aliases: ["aeolian"] },
  { name: "dorian", intervals: "1P 2M 3m 4P 5P 6M 7m" },
  { name: "mixolydian", intervals: "1P 2M 3M 4P 5P 6M 7m" },
  { name: "harmonic_minor", intervals: "1P 2M 3m 4P 5P 6m 7M" },
  { name: "major_pentatonic", intervals: "1P 2M 3M 5P 6M" },
  { name: "minor_pentatonic", intervals: "1P 3m 4P 5P 7m" },
  { name: "chromatic", intervals: "1P 2m 2M 3m 3M 4P 4A 5P 6m 6M 7m 7M" },
];

// ----------------------------------------------------------------------------
// Roots (pitch class 0..11) — names match the user-typed scale string.
// ----------------------------------------------------------------------------
//
// Both flat-style ("eb") and sharp-style ("d#") names are emitted so users
// don't have to remember which spelling the catalog ships.

// Sharp-only canonical roots — flat aliases (eb, ab, …) are dropped for v1
// to keep the embedded stdlib under the constexpr loop limit. The interval
// catalog is enharmonically complete; "d#:minor" and "eb:minor" name the
// same scale.
const ROOTS: { name: string; pc: number }[] = [
  { name: "c",  pc: 0 },
  { name: "c#", pc: 1 },
  { name: "d",  pc: 2 },
  { name: "d#", pc: 3 },
  { name: "e",  pc: 4 },
  { name: "f",  pc: 5 },
  { name: "f#", pc: 6 },
  { name: "g",  pc: 7 },
  { name: "g#", pc: 8 },
  { name: "a",  pc: 9 },
  { name: "a#", pc: 10 },
  { name: "b",  pc: 11 },
];

// Octaves emitted for `scale` AND as ignored aliases for `key` (the octave
// digit is accepted and discarded per PRD §4.3, so "d2:minor" quantizes
// identically to "d:minor"). Octaves 2..4 cover the practical live-coding
// range (below C2 ≈ 65 Hz is rare; above C5 ≈ 523 Hz the degree mapping
// clamps quickly against MIDI 127). The octave-less default is
// "root:scale" -> octave 3.
const SCALE_OCTAVES = [2, 3, 4];
const SCALE_DEFAULT_OCTAVE = 3;

// ----------------------------------------------------------------------------
// Per-(root, scale) precomputation.
// ----------------------------------------------------------------------------

/** Rotate scale intervals to a given root pitch class — pitch-class set in P. */
function pitchClassSet(intervals: number[], rootPc: number): Set<number> {
  return new Set(intervals.map((i) => (i + rootPc) % 12));
}

/**
 * 12-entry delta table for `key`: for each input pitch class 0..11, the
 * signed-semitone delta to the nearest pitch class in `P`. Tie → lower MIDI
 * (negative delta wins). Per PRD §4.5 + §11.4.
 */
function keyDeltaTable(intervals: number[], rootPc: number): number[] {
  const P = pitchClassSet(intervals, rootPc);
  const deltas: number[] = [];
  for (let pc = 0; pc < 12; pc++) {
    if (P.has(pc)) {
      deltas.push(0);
      continue;
    }
    let best: number | null = null;
    for (let d = 1; d <= 6; d++) {
      const upPc = (pc + d) % 12;
      const downPc = (pc - d + 12) % 12;
      const upIn = P.has(upPc);
      const downIn = P.has(downPc);
      if (downIn && upIn) { best = -d; break; }     // tie → lower
      if (downIn)        { best = -d; break; }
      if (upIn)          { best = d;  break; }
    }
    deltas.push(best ?? 0);
  }
  return deltas;
}

/** root_midi = pc + 12·(octave + 1) per PRD §4.4. */
function rootMidi(rootPc: number, octave: number): number {
  return rootPc + 12 * (octave + 1);
}

// ----------------------------------------------------------------------------
// Emission
// ----------------------------------------------------------------------------

function fmtArray(xs: number[]): string {
  return "[" + xs.join(", ") + "]";
}

function emitScaleQuantizeAk(): string {
  const lines: string[] = [];
  lines.push("// Akkado stdlib — scale quantization (auto-generated).");
  lines.push("//");
  lines.push("// AUTO-GENERATED FILE — DO NOT EDIT BY HAND.");
  lines.push("//");
  lines.push("// Generated from web/scripts/generate-scale-quantize.ts. To regenerate:");
  lines.push("//     cd web && bun run scripts/generate-scale-quantize.ts");
  lines.push("//");
  lines.push("// Implements docs/prd-scale-quantize.md §4.4 (`scale`, degree mapping)");
  lines.push("// and §4.5 (`key`, 12-TET quantization). Both are the outer `match`");
  lines.push("// themselves — the literal-only constraint of match dispatchers (see");
  lines.push("// scales.ak header) precludes delegating to `scale_intervals(name)`");
  lines.push("// at runtime, so each branch inlines its own literal interval array.");
  lines.push("//");
  lines.push("// Catalog: 12 chromatic roots × " + SCALES.length + " scale types.");
  lines.push("// `key` strings are octave-agnostic — an octave digit is accepted and");
  lines.push("// ignored (\"d2:minor\" == \"d:minor\"). `scale` strings use the octave");
  lines.push("// digit (e.g. `\"d3:minor\"`), with a missing digit defaulting to");
  lines.push("// octave 3 per PRD §4.3.");
  lines.push("");
  lines.push("// ----------------------------------------------------------------------------");
  lines.push("// Helper fns — factored arithmetic shared across branches");
  lines.push("// ----------------------------------------------------------------------------");
  lines.push("//");
  lines.push("// `snap` round-halves-up a (possibly fractional) MIDI note to the nearest");
  lines.push("// integer. `pc12` reduces a (possibly negative) MIDI note to its pitch");
  lines.push("// class 0..11, two `fmod`s to handle negative inputs symmetrically.");
  lines.push("//");
  lines.push("// `key_q` / `scale_q` carry the entire per-event arithmetic ONCE; every");
  lines.push("// match branch is a short call passing literal tables. The match-dispatcher");
  lines.push("// literal-only constraint applies to the match SCRUTINEE (the name string),");
  lines.push("// not to delegating the arithmetic — after the match folds to one branch,");
  lines.push("// what remains is a plain fn call with literal args, and closures capture");
  lines.push("// fn params exactly like stdlib `transpose` does. This keeps the embedded");
  lines.push("// stdlib small: per-compile parse cost scales with file size.");
  lines.push("");
  lines.push("fn snap(n) -> floor(n + 0.5)");
  lines.push("fn pc12(n) -> fmod(fmod(n, 12) + 12, 12)");
  lines.push("");
  lines.push("fn key_q(events: Stream, deltas) -> event_map(events, (e) -> {note: snap(e.note) + deltas[pc12(snap(e.note))]})");
  lines.push("fn scale_q(events: Stream, rm, k, ivals) -> event_map(events, (e) -> {note: rm + floor(snap(e.note) / k) * 12 + ivals[snap(e.note) - floor(snap(e.note) / k) * k]})");
  lines.push("");

  // -------- `fn note_num` --------
  lines.push("// ----------------------------------------------------------------------------");
  lines.push("// fn note_num — note-name string to MIDI number. Octave-less names default");
  lines.push("// to octave 3 (\"d\" -> 50), matching scale()'s default. Numbers pass");
  lines.push("// through the `_` arm untouched, so callers may hand a MIDI number");
  lines.push("// directly wherever a root name is accepted.");
  lines.push("// ----------------------------------------------------------------------------");
  lines.push("");
  lines.push("fn note_num(name) -> match(name) {");
  for (const root of ROOTS) {
    lines.push(
      `    "${root.name}": ${rootMidi(root.pc, SCALE_DEFAULT_OCTAVE)}`,
    );
    for (let oct = 0; oct <= 8; oct++) {
      lines.push(`    "${root.name}${oct}": ${rootMidi(root.pc, oct)}`);
    }
  }
  lines.push("    _: name");
  lines.push("}");
  lines.push("");

  // -------- user-defined (root, intervals) overloads (PRD §4.6) --------
  lines.push("// ----------------------------------------------------------------------------");
  lines.push("// User-defined scales (PRD §4.6) — arity-3 overloads. `root` is a note");
  lines.push("// name string (\"d2\") or a MIDI number; `ivals` a semitone interval");
  lines.push("// array. key_deltas() is a compile-time builtin (C++ const-eval) that");
  lines.push("// turns (root, intervals) into the 12-entry nearest-tone delta table.");
  lines.push("// No validation by design — out-of-range intervals coerce mod 12");
  lines.push("// (live-coding coerce-don't-fail; audit 2026-07-04).");
  lines.push("// ----------------------------------------------------------------------------");
  lines.push("");
  lines.push("// key_deltas resolves the root itself (note-name string or number,");
  lines.push("// octave ignored). Both bodies bind the compile-time pieces to LOCALS");
  lines.push("// before the nested call: codegen clears param_literals_ around a");
  lines.push("// nested call's argument list, so key_deltas(root, ...) / note_num(root)");
  lines.push("// can only see `root`'s caller literal from body-statement position.");
  lines.push("fn key(events: Stream, root, ivals) -> {");
  lines.push("    deltas = key_deltas(root, ivals)");
  lines.push("    key_q(events, deltas)");
  lines.push("}");
  lines.push("fn scale(events: Stream, root, ivals) -> {");
  lines.push("    rm = note_num(root)");
  lines.push("    scale_q(events, rm, len(ivals), ivals)");
  lines.push("}");
  lines.push("");

  // -------- `fn key` --------
  lines.push("// ----------------------------------------------------------------------------");
  lines.push("// fn key — octave-agnostic 12-TET quantization. Per-scale delta tables");
  lines.push("// computed at generation time (see web/scripts/generate-scale-quantize.ts");
  lines.push("// `keyDeltaTable`).");
  lines.push("// ----------------------------------------------------------------------------");
  lines.push("");
  lines.push("fn key(events: Stream, name) -> match(name) {");

  // Each branch delegates to `key_q` with its literal delta table.
  const keyBranches: string[] = [];
  for (const root of ROOTS) {
    for (const scale of SCALES) {
      const delta = keyDeltaTable(intervalsToSemitones(scale.intervals), root.pc);
      const call = `key_q(events, ${fmtArray(delta)})`;
      // Octave digits are accepted and IGNORED per PRD §4.3/§8 — every
      // "rootN:scale" alias shares the octave-less branch call. Without
      // these aliases a scale()-style string like "d2:minor" silently
      // fell through to identity (audit 2026-07-04).
      const names = [scale.name, ...(scale.aliases ?? [])];
      for (const n of names) {
        keyBranches.push(`    "${root.name}:${n}": ${call}`);
        for (const oct of SCALE_OCTAVES) {
          keyBranches.push(`    "${root.name}${oct}:${n}": ${call}`);
        }
      }
    }
  }
  lines.push(keyBranches.join("\n"));
  lines.push("    _: events");
  lines.push("}");
  lines.push("");

  // -------- `fn scale` --------
  lines.push("// ----------------------------------------------------------------------------");
  lines.push("// fn scale — degree-mapping. The name string carries the octave digit");
  lines.push("// (e.g. \"d3:minor\"). A missing octave defaults to 3 — both \"d:minor\"");
  lines.push("// and \"d3:minor\" land on root MIDI 50.");
  lines.push("// ----------------------------------------------------------------------------");
  lines.push("");
  lines.push("fn scale(events: Stream, name) -> match(name) {");

  // Each branch delegates to `scale_q(events, root_midi, k, intervals)`.
  const scaleBranches: string[] = [];
  for (const root of ROOTS) {
    for (const scale of SCALES) {
      const intervals = intervalsToSemitones(scale.intervals);
      const k = intervals.length;
      const intervalsLit = fmtArray(intervals);
      const callFor = (oct: number) =>
        `scale_q(events, ${rootMidi(root.pc, oct)}, ${k}, ${intervalsLit})`;

      const names = [scale.name, ...(scale.aliases ?? [])];
      for (const n of names) {
        // Octave-less form defaults to octave 3.
        scaleBranches.push(
          `    "${root.name}:${n}": ${callFor(SCALE_DEFAULT_OCTAVE)}`,
        );
        for (const oct of SCALE_OCTAVES) {
          scaleBranches.push(
            `    "${root.name}${oct}:${n}": ${callFor(oct)}`,
          );
        }
      }
    }
  }
  lines.push(scaleBranches.join("\n"));
  lines.push("    _: events");
  lines.push("}");
  lines.push("");

  return lines.join("\n");
}

// ----------------------------------------------------------------------------
// Spot-check
// ----------------------------------------------------------------------------

function spotCheck(): void {
  // Interval sets pinned against tonal.js scale-type data (PRD §10:
  // "spot-check scales vs tonal.js"). Covers the entire shipped catalog.
  const EXPECTED_INTERVALS: Record<string, number[]> = {
    major: [0, 2, 4, 5, 7, 9, 11],
    minor: [0, 2, 3, 5, 7, 8, 10],
    dorian: [0, 2, 3, 5, 7, 9, 10],
    mixolydian: [0, 2, 4, 5, 7, 9, 10],
    harmonic_minor: [0, 2, 3, 5, 7, 8, 11],
    major_pentatonic: [0, 2, 4, 7, 9],
    minor_pentatonic: [0, 3, 5, 7, 10],
    chromatic: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11],
  };
  for (const scale of SCALES) {
    const got = intervalsToSemitones(scale.intervals);
    const want = EXPECTED_INTERVALS[scale.name];
    if (!want) throw new Error(`no expected intervals pinned for '${scale.name}'`);
    if (got.length !== want.length || got.some((v, i) => v !== want[i])) {
      throw new Error(
        `${scale.name} intervals [${got}] != tonal.js [${want}]`,
      );
    }
  }

  // Key-delta invariants for every (root, scale): delta 0 iff pc ∈ P,
  // |delta| is the true minimum pc distance, tie → negative (lower MIDI,
  // PRD §11.4).
  for (const root of ROOTS) {
    for (const scale of SCALES) {
      const intervals = intervalsToSemitones(scale.intervals);
      const P = pitchClassSet(intervals, root.pc);
      const deltas = keyDeltaTable(intervals, root.pc);
      for (let pc = 0; pc < 12; pc++) {
        const d = deltas[pc];
        const where = `${root.name}:${scale.name} pc=${pc}`;
        if (P.has(pc)) {
          if (d !== 0) throw new Error(`${where}: in-scale pc has delta ${d}`);
          continue;
        }
        if (d === 0) throw new Error(`${where}: out-of-scale pc has delta 0`);
        if (!P.has((((pc + d) % 12) + 12) % 12)) {
          throw new Error(`${where}: delta ${d} lands outside P`);
        }
        for (let closer = 1; closer < Math.abs(d); closer++) {
          if (P.has((pc + closer) % 12) || P.has((pc - closer + 12) % 12)) {
            throw new Error(`${where}: delta ${d} not minimal (dist ${closer} exists)`);
          }
        }
        if (d > 0 && P.has((pc - d + 12) % 12)) {
          throw new Error(`${where}: tie broken upward (delta ${d}), must go lower`);
        }
      }
    }
  }

  const cMajorDelta = keyDeltaTable(intervalsToSemitones(SCALES[0].intervals), 0);
  const expected = [0, -1, 0, -1, 0, 0, -1, 0, -1, 0, -1, 0];
  for (let i = 0; i < 12; i++) {
    if (cMajorDelta[i] !== expected[i]) {
      throw new Error(`c:major delta[${i}] = ${cMajorDelta[i]}, expected ${expected[i]}`);
    }
  }
  if (rootMidi(2, 3) !== 50) throw new Error(`D3 should be MIDI 50, got ${rootMidi(2, 3)}`);
  if (rootMidi(0, 4) !== 60) throw new Error(`C4 should be MIDI 60, got ${rootMidi(0, 4)}`);
  console.log(
    `Spot-check OK (${SCALES.length} interval sets vs tonal.js, ` +
    `${ROOTS.length * SCALES.length} key-delta tables validated)`,
  );
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------

spotCheck();
const out = emitScaleQuantizeAk();
writeFileSync(OUTPUT_FILE, out);
console.log(
  `Wrote ${OUTPUT_FILE} (${out.length} bytes, ${ROOTS.length} roots × ${SCALES.length} scales)`,
);
