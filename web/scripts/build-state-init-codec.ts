/**
 * Generate the state-init wire-format validator codec (TypeScript).
 *
 * This script parses the authoritative wire-format facts out of the C++
 * headers and emits web/src/lib/audio/state-init-codec.ts — a *decoder* +
 * size constants used by web/tests/state-init-codec.test.ts to cross-check
 * the real C++-packed buffers (produced by the worker's WASM packer) against
 * the layout declared in C++.
 *
 * The worker still packs in C++ (akkado/include/akkado/state_init_buffer.hpp);
 * this TS codec never packs production data — it exists to catch wire-format
 * drift between the two WASM builds at test time. See
 * docs/prd-compile-off-audio-thread.md §5.4.
 *
 * Sources parsed:
 * - akkado/include/akkado/state_init_buffer.hpp
 *     MAGIC_STATE_INITS, MAGIC_MIDI_SOURCES, WIRE_VERSION, RecordKind enum,
 *     static_assert(sizeof(cedar::Event)==N), static_assert(Breakpoint==N)
 * - akkado/include/akkado/codegen.hpp
 *     StateInitData::Type enum (record-type discriminants)
 * - cedar/include/cedar/vm/program_slot.hpp
 *     static_assert(sizeof(BlockEntry)==N)
 *
 * Usage: bun run scripts/build-state-init-codec.ts  (== bun run build:state-init-codec)
 */

import { readFileSync, writeFileSync } from "fs";
import { resolve } from "path";

const ROOT_DIR = resolve(import.meta.dir, "../..");
const STATE_INIT_BUFFER_HPP = resolve(
  ROOT_DIR,
  "akkado/include/akkado/state_init_buffer.hpp"
);
const CODEGEN_HPP = resolve(ROOT_DIR, "akkado/include/akkado/codegen.hpp");
const PROGRAM_SLOT_HPP = resolve(
  ROOT_DIR,
  "cedar/include/cedar/vm/program_slot.hpp"
);
const OUTPUT_FILE = resolve(
  ROOT_DIR,
  "web/src/lib/audio/state-init-codec.ts"
);

function fail(msg: string): never {
  throw new Error(`[build-state-init-codec] ${msg}`);
}

/** Parse an `inline constexpr <type> NAME = 0x...u;` value as a number. */
function parseConstexprHex(source: string, name: string): number {
  const re = new RegExp(
    `constexpr\\s+\\S+\\s+${name}\\s*=\\s*(0x[0-9A-Fa-f]+)u?`
  );
  const m = source.match(re);
  if (!m) fail(`could not find constexpr ${name}`);
  return parseInt(m[1], 16);
}

/** Parse an `inline constexpr <type> NAME = <int>;` value as a number. */
function parseConstexprInt(source: string, name: string): number {
  const re = new RegExp(`constexpr\\s+\\S+\\s+${name}\\s*=\\s*(\\d+)`);
  const m = source.match(re);
  if (!m) fail(`could not find constexpr ${name}`);
  return parseInt(m[1], 10);
}

/**
 * Parse `static_assert(sizeof(<expr>) == <N>` → N. Tolerates the type being
 * written with or without a leading `cedar::` namespace qualifier (the pins
 * live in different headers and quote it both ways).
 */
function parseSizeofAssert(source: string, expr: string): number {
  const candidates = [expr, expr.replace(/^cedar::/, "")];
  for (const cand of candidates) {
    const esc = cand.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    const re = new RegExp(
      `static_assert\\s*\\(\\s*sizeof\\s*\\(\\s*${esc}\\s*\\)\\s*==\\s*(\\d+)`
    );
    const m = source.match(re);
    if (m) return parseInt(m[1], 10);
  }
  return fail(`could not find static_assert(sizeof(${expr}) == N)`);
}

/** Parse a `enum class <Name> : <type> { ... }` body into ordered entries. */
function parseEnum(
  source: string,
  enumName: string
): Array<{ name: string; value: number }> {
  const re = new RegExp(
    `enum\\s+class\\s+${enumName}\\s*:\\s*[\\w:]+\\s*\\{([\\s\\S]*?)\\}`
  );
  const m = source.match(re);
  if (!m) fail(`could not find enum class ${enumName}`);
  const body = m[1];

  const entries: Array<{ name: string; value: number }> = [];
  let next = 0;
  for (const rawLine of body.split("\n")) {
    // Strip line + block comment fragments.
    const line = rawLine.replace(/\/\/.*$/, "").replace(/\/\*[\s\S]*?\*\//g, "");
    for (const piece of line.split(",")) {
      const t = piece.trim();
      if (!t) continue;
      const am = t.match(/^([A-Za-z_]\w*)\s*(?:=\s*(\d+))?$/);
      if (!am) continue;
      const name = am[1];
      const value = am[2] !== undefined ? parseInt(am[2], 10) : next;
      entries.push({ name, value });
      next = value + 1;
    }
  }
  if (entries.length === 0) fail(`enum class ${enumName} parsed to 0 entries`);
  return entries;
}

const stateInitBufferSrc = readFileSync(STATE_INIT_BUFFER_HPP, "utf-8");
const codegenSrc = readFileSync(CODEGEN_HPP, "utf-8");
const programSlotSrc = readFileSync(PROGRAM_SLOT_HPP, "utf-8");

const MAGIC_STATE_INITS = parseConstexprHex(
  stateInitBufferSrc,
  "MAGIC_STATE_INITS"
);
const MAGIC_MIDI_SOURCES = parseConstexprHex(
  stateInitBufferSrc,
  "MAGIC_MIDI_SOURCES"
);
const WIRE_VERSION = parseConstexprInt(stateInitBufferSrc, "WIRE_VERSION");
const recordKinds = parseEnum(stateInitBufferSrc, "RecordKind");
const stateInitTypes = parseEnum(codegenSrc, "Type");

const EVENT_SIZE = parseSizeofAssert(stateInitBufferSrc, "cedar::Event");
const BREAKPOINT_SIZE = parseSizeofAssert(
  stateInitBufferSrc,
  "cedar::TimelineState::Breakpoint"
);
const blockEntrySize = parseSizeofAssert(programSlotSrc, "cedar::BlockEntry");

function enumToTs(
  entries: Array<{ name: string; value: number }>
): string {
  return entries.map((e) => `  ${e.name} = ${e.value},`).join("\n");
}

const banner = `// AUTO-GENERATED FILE — DO NOT EDIT.
// Generated by web/scripts/build-state-init-codec.ts
// To regenerate, run: cd web && bun run build:state-init-codec
//
// Wire-format validator for the state-init buffers produced by the C++ packer
// in akkado/include/akkado/state_init_buffer.hpp (PRD §5). This is a DECODER
// only — the worker packs production data in C++/WASM, never here. The decoder
// + size constants are cross-checked against real C++-packed bytes by
// web/tests/state-init-codec.test.ts, which fails if the wire format ever
// drifts from this generated description.`;

const output = `${banner}

export const WIRE_VERSION = ${WIRE_VERSION};
export const MAGIC_STATE_INITS = 0x${MAGIC_STATE_INITS.toString(16).toUpperCase()};
export const MAGIC_MIDI_SOURCES = 0x${MAGIC_MIDI_SOURCES.toString(16).toUpperCase()};

// Byte sizes of the structs the wire format embeds via raw memcpy. Pinned in
// C++ via static_assert; any change there changes these and must bump
// WIRE_VERSION.
export const EVENT_SIZE = ${EVENT_SIZE};
export const BREAKPOINT_SIZE = ${BREAKPOINT_SIZE};
export const BLOCK_ENTRY_SIZE = ${blockEntrySize};
// kind:u8 + pad[3] + state_id:u32 + payload_size:u32
export const RECORD_HEADER_SIZE = 12;
// magic:u32 + version:u16 + record_count:u16
export const BUFFER_HEADER_SIZE = 8;

export enum RecordKind {
${enumToTs(recordKinds)}
}

export enum StateInitType {
${enumToTs(stateInitTypes)}
}

export interface SequenceSummary {
  duration: number;
  mode: number;
  numEvents: number;
}

export interface StateInitRecord {
  kind: RecordKind;
  stateId: number;
  payloadSize: number;
  /** present when kind === RecordKind.StateInit */
  type?: StateInitType;
  /** present for StateInitType.SequenceProgram */
  cycleLength?: number;
  isSamplePattern?: boolean;
  totalEvents?: number;
  numSequences?: number;
  sequences?: SequenceSummary[];
}

export interface DecodedStateInits {
  magic: number;
  version: number;
  recordCount: number;
  records: StateInitRecord[];
  /** total bytes consumed by the walk; equals buf.byteLength iff layout matches */
  bytesConsumed: number;
}

export interface MidiSourceRecord {
  stateId: number;
  kind: number;
  channelFilter: number;
  loop: boolean;
  tempoMode: number;
  name: string;
}

export interface DecodedMidiSources {
  magic: number;
  version: number;
  recordCount: number;
  records: MidiSourceRecord[];
  bytesConsumed: number;
}

/**
 * Decode a stateInitsBuf (§5.1–5.3). Walks every record using the on-wire
 * payload_size, and for SequenceProgram records steps over events using
 * EVENT_SIZE — so a wrong EVENT_SIZE makes the per-sequence walk overrun the
 * record's declared payload_size, which the caller can detect.
 */
export function decodeStateInits(buf: Uint8Array): DecodedStateInits {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  if (buf.byteLength < BUFFER_HEADER_SIZE) {
    throw new Error("stateInitsBuf shorter than header");
  }
  const magic = dv.getUint32(0, true);
  const version = dv.getUint16(4, true);
  const recordCount = dv.getUint16(6, true);

  const records: StateInitRecord[] = [];
  let off = BUFFER_HEADER_SIZE;
  for (let r = 0; r < recordCount; r++) {
    if (off + RECORD_HEADER_SIZE > buf.byteLength) {
      throw new Error(\`record \${r} header overruns buffer\`);
    }
    const kind = dv.getUint8(off) as RecordKind;
    const stateId = dv.getUint32(off + 4, true);
    const payloadSize = dv.getUint32(off + 8, true);
    const payloadStart = off + RECORD_HEADER_SIZE;
    const payloadEnd = payloadStart + payloadSize;
    if (payloadEnd > buf.byteLength) {
      throw new Error(\`record \${r} payload overruns buffer\`);
    }

    const rec: StateInitRecord = { kind, stateId, payloadSize };

    if (kind === RecordKind.StateInit) {
      // Payload starts with type:u8 + pad[3].
      const type = dv.getUint8(payloadStart) as StateInitType;
      rec.type = type;
      let p = payloadStart + 4;
      if (type === StateInitType.SequenceProgram) {
        rec.cycleLength = dv.getFloat32(p, true);
        p += 4;
        rec.isSamplePattern = dv.getUint8(p) !== 0;
        p += 4; // u8 + pad[3]
        rec.totalEvents = dv.getUint32(p, true);
        p += 4;
        const numSequences = dv.getUint32(p, true);
        p += 4;
        rec.numSequences = numSequences;
        const sequences: SequenceSummary[] = [];
        for (let s = 0; s < numSequences; s++) {
          const duration = dv.getFloat32(p, true);
          p += 4;
          const mode = dv.getUint8(p);
          p += 4; // u8 + pad[3]
          const numEvents = dv.getUint32(p, true);
          p += 4;
          p += numEvents * EVENT_SIZE; // skip the memcpy'd cedar::Event[]
          sequences.push({ duration, mode, numEvents });
        }
        rec.sequences = sequences;
        if (p !== payloadEnd) {
          throw new Error(
            \`SequenceProgram record \${r} walked to \${p}, expected \${payloadEnd} \` +
              \`(EVENT_SIZE/layout drift?)\`
          );
        }
      }
    }

    records.push(rec);
    off = payloadEnd;
  }

  return { magic, version, recordCount, records, bytesConsumed: off };
}

/** Decode a midiSourcesBuf (§5.6). */
export function decodeMidiSources(buf: Uint8Array): DecodedMidiSources {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  if (buf.byteLength < BUFFER_HEADER_SIZE) {
    throw new Error("midiSourcesBuf shorter than header");
  }
  const magic = dv.getUint32(0, true);
  const version = dv.getUint16(4, true);
  const recordCount = dv.getUint16(6, true);

  const decoder = new TextDecoder();
  const records: MidiSourceRecord[] = [];
  let off = BUFFER_HEADER_SIZE;
  for (let r = 0; r < recordCount; r++) {
    const stateId = dv.getUint32(off, true);
    const kind = dv.getUint8(off + 4);
    const channelFilter = dv.getUint8(off + 5);
    const loop = dv.getUint8(off + 6) !== 0;
    const tempoMode = dv.getUint8(off + 7);
    const nameLen = dv.getUint16(off + 8, true);
    // off+10..off+12: pad[2]
    const nameStart = off + 12;
    const name = decoder.decode(buf.subarray(nameStart, nameStart + nameLen));
    let next = nameStart + nameLen;
    // Records are padded to a 4-byte boundary by the packer.
    while (next % 4 !== 0) next++;
    records.push({ stateId, kind, channelFilter, loop, tempoMode, name });
    off = next;
  }

  return { magic, version, recordCount, records, bytesConsumed: off };
}
`;

writeFileSync(OUTPUT_FILE, output);
console.log(
  `[build-state-init-codec] wrote ${OUTPUT_FILE}\n` +
    `  WIRE_VERSION=${WIRE_VERSION} EVENT_SIZE=${EVENT_SIZE} ` +
    `BREAKPOINT_SIZE=${BREAKPOINT_SIZE} BLOCK_ENTRY_SIZE=${blockEntrySize}\n` +
    `  RecordKind=${recordKinds.length} StateInitType=${stateInitTypes.length}`
);
