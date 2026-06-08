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

// Octaves emitted for `scale`. `key` ignores octave and emits only the
// octave-less form (octave param is discarded per PRD §4.3). Octaves 2..4
// cover the practical live-coding range (below C2 ≈ 65 Hz is rare; above C5
// ≈ 523 Hz the degree mapping clamps quickly against MIDI 127). The
// octave-less default is "root:scale" -> octave 3.
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
  lines.push("// `key` strings are octave-agnostic (root only); `scale` strings carry");
  lines.push("// the octave digit (e.g. `\"d3:minor\"`), with a missing digit defaulting");
  lines.push("// to octave 3 per PRD §4.3.");
  lines.push("");
  lines.push("// ----------------------------------------------------------------------------");
  lines.push("// Helper fns — factored arithmetic shared across branches");
  lines.push("// ----------------------------------------------------------------------------");
  lines.push("//");
  lines.push("// `snap` round-halves-up a (possibly fractional) MIDI note to the nearest");
  lines.push("// integer. `pc12` reduces a (possibly negative) MIDI note to its pitch");
  lines.push("// class 0..11, two `fmod`s to handle negative inputs symmetrically.");
  lines.push("// These are called from every match-branch closure body and lower to a");
  lines.push("// single shared `fn` body via BLOCK_CALL.");
  lines.push("");
  lines.push("fn snap(n) -> floor(n + 0.5)");
  lines.push("fn pc12(n) -> fmod(fmod(n, 12) + 12, 12)");
  lines.push("");

  // -------- `fn key` --------
  lines.push("// ----------------------------------------------------------------------------");
  lines.push("// fn key — octave-agnostic 12-TET quantization. Per-scale delta tables");
  lines.push("// computed at generation time (see web/scripts/generate-scale-quantize.ts");
  lines.push("// `keyDeltaTable`).");
  lines.push("// ----------------------------------------------------------------------------");
  lines.push("");
  lines.push("fn key(events: Stream, name) -> match(name) {");

  // Each branch:
  //   snap(e.note) + delta_table[ pc12(snap(e.note)) ]
  // `snap` is shared via fn — lowers to BLOCK_CALL when called from >=2 sites.
  function keySnapBody(deltaArr: string): string {
    return "snap(e.note) + " + deltaArr + "[pc12(snap(e.note))]";
  }

  const keyBranches: string[] = [];
  for (const root of ROOTS) {
    for (const scale of SCALES) {
      const delta = keyDeltaTable(intervalsToSemitones(scale.intervals), root.pc);
      const body = keySnapBody(fmtArray(delta));
      const k = `"${root.name}:${scale.name}"`;
      keyBranches.push(
        `    ${k}: event_map(events, (e) -> {note: ${body}})`,
      );
      for (const alias of scale.aliases ?? []) {
        const ak = `"${root.name}:${alias}"`;
        keyBranches.push(
          `    ${ak}: event_map(events, (e) -> {note: ${body}})`,
        );
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

  // Per-branch:
  //   root_midi
  //     + floor(snap(e.note) / k) * 12
  //     + intervals[ snap(e.note) - floor(snap(e.note) / k) * k ]
  // `snap` is shared via fn — calls lower to BLOCK_CALL.
  function scaleStepBody(rm: number, intervalsLit: string, k: number): string {
    return (
      rm +
      " + floor(snap(e.note) / " + k + ") * 12 + " +
      intervalsLit +
      "[snap(e.note) - floor(snap(e.note) / " + k + ") * " + k + "]"
    );
  }

  const scaleBranches: string[] = [];
  for (const root of ROOTS) {
    for (const scale of SCALES) {
      const intervals = intervalsToSemitones(scale.intervals);
      const k = intervals.length;
      const intervalsLit = fmtArray(intervals);

      // Octave-less form defaults to octave 3.
      const rmDefault = rootMidi(root.pc, SCALE_DEFAULT_OCTAVE);
      const bodyDefault = scaleStepBody(rmDefault, intervalsLit, k);
      scaleBranches.push(
        `    "${root.name}:${scale.name}": event_map(events, (e) -> {note: ${bodyDefault}})`,
      );

      // Explicit-octave forms.
      for (const oct of SCALE_OCTAVES) {
        const rm = rootMidi(root.pc, oct);
        const body = scaleStepBody(rm, intervalsLit, k);
        scaleBranches.push(
          `    "${root.name}${oct}:${scale.name}": event_map(events, (e) -> {note: ${body}})`,
        );
      }

      // Aliases.
      for (const alias of scale.aliases ?? []) {
        scaleBranches.push(
          `    "${root.name}:${alias}": event_map(events, (e) -> {note: ${bodyDefault}})`,
        );
        for (const oct of SCALE_OCTAVES) {
          const rm = rootMidi(root.pc, oct);
          const body = scaleStepBody(rm, intervalsLit, k);
          scaleBranches.push(
            `    "${root.name}${oct}:${alias}": event_map(events, (e) -> {note: ${body}})`,
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
  const cMajorDelta = keyDeltaTable(intervalsToSemitones(SCALES[0].intervals), 0);
  const expected = [0, -1, 0, -1, 0, 0, -1, 0, -1, 0, -1, 0];
  for (let i = 0; i < 12; i++) {
    if (cMajorDelta[i] !== expected[i]) {
      throw new Error(`c:major delta[${i}] = ${cMajorDelta[i]}, expected ${expected[i]}`);
    }
  }
  if (rootMidi(2, 3) !== 50) throw new Error(`D3 should be MIDI 50, got ${rootMidi(2, 3)}`);
  if (rootMidi(0, 4) !== 60) throw new Error(`C4 should be MIDI 60, got ${rootMidi(0, 4)}`);
  console.log("Spot-check OK (c:major delta + root_midi sanity)");
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
