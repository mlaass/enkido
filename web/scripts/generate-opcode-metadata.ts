/**
 * Generate opcode metadata header from source files
 *
 * This script parses:
 * - cedar/include/cedar/vm/instruction.hpp (Opcode enum)
 * - akkado/include/akkado/builtins.hpp (requires_state flags)
 *
 * And generates:
 * - cedar/include/cedar/generated/opcode_metadata.hpp
 *
 * Usage: bun run scripts/generate-opcode-metadata.ts
 */

import { readFileSync, writeFileSync, mkdirSync, existsSync } from "fs";
import { resolve, dirname } from "path";

const ROOT_DIR = resolve(import.meta.dir, "../..");
const INSTRUCTION_HPP = resolve(ROOT_DIR, "cedar/include/cedar/vm/instruction.hpp");
const BUILTINS_HPP = resolve(ROOT_DIR, "akkado/include/akkado/builtins.hpp");
const OUTPUT_DIR = resolve(ROOT_DIR, "cedar/include/cedar/generated");
const OUTPUT_FILE = resolve(OUTPUT_DIR, "opcode_metadata.hpp");

interface OpcodeInfo {
  name: string;
  value: number;
  isStateful: boolean;
}

/**
 * Parse the Opcode enum from instruction.hpp
 */
function parseOpcodeEnum(source: string): Map<string, number> {
  const opcodes = new Map<string, number>();

  // Find the enum class Opcode block
  const enumMatch = source.match(/enum\s+class\s+Opcode\s*:\s*std::uint8_t\s*\{([^}]+)\}/s);
  if (!enumMatch) {
    throw new Error("Could not find Opcode enum in instruction.hpp");
  }

  const enumBody = enumMatch[1];

  // Match each enum value: NAME = VALUE or just NAME (auto-increment)
  // Handle comments on the same line
  const lines = enumBody.split("\n");
  let currentValue = 0;

  for (const line of lines) {
    // Skip comment-only lines and empty lines
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith("//")) continue;

    // Extract the enum entry, handling inline comments
    const entryMatch = trimmed.match(/^(\w+)\s*(?:=\s*(\d+))?\s*,?/);
    if (entryMatch) {
      const name = entryMatch[1];
      if (entryMatch[2] !== undefined) {
        currentValue = parseInt(entryMatch[2], 10);
      }
      opcodes.set(name, currentValue);
      currentValue++;
    }
  }

  return opcodes;
}

/**
 * Parse the BUILTIN_FUNCTIONS map to extract which opcodes are stateful
 */
function parseStatefulOpcodes(source: string): Set<string> {
  const stateful = new Set<string>();

  // Find the BUILTIN_FUNCTIONS map
  const mapMatch = source.match(/BUILTIN_FUNCTIONS\s*=\s*\{([\s\S]*?)\n\};/);
  if (!mapMatch) {
    throw new Error("Could not find BUILTIN_FUNCTIONS in builtins.hpp");
  }

  const mapBody = mapMatch[1];

  // Match each builtin entry to extract opcode and requires_state
  // Pattern: {cedar::Opcode::OPCODE_NAME, input_count, optional_count, true/false,
  const entryRegex = /\{cedar::Opcode::(\w+),\s*\d+,\s*\d+,\s*(true|false),/g;
  let match;

  while ((match = entryRegex.exec(mapBody)) !== null) {
    const opcodeName = match[1];
    const requiresState = match[2] === "true";
    if (requiresState) {
      stateful.add(opcodeName);
    }
  }

  return stateful;
}

/**
 * Infer statefulness for opcodes not in builtins based on naming patterns
 * This catches oversampled oscillators and other opcodes not directly exposed
 */
function inferStatefulOpcodes(opcodes: Map<string, number>, fromBuiltins: Set<string>): Set<string> {
  const stateful = new Set(fromBuiltins);

  // Patterns that indicate statefulness
  const statefulPrefixes = [
    "OSC_",      // All oscillators maintain phase
    "FILTER_",   // All filters maintain state
    "REVERB_",   // Reverbs have delay lines
    "EFFECT_",   // Effects have delay lines/LFOs
    "DELAY",     // Delay lines
    "ENV_ADSR",  // Envelopes
    "ENV_AR",
    "ENV_FOLLOWER",
    "SAMPLE_",   // Samplers track playback position
    "DYNAMICS_", // Dynamics processors have envelope followers
    "SEQPAT_",   // Sequence pattern opcodes track state
  ];

  // Individual opcodes that are stateful but might not match patterns
  const explicitStateful = [
    "NOISE",     // RNG state
    "SLEW",      // Tracks current value
    "SAH",       // Sample and hold
    "LFO",       // Phase state
    "SEQ_STEP",  // Step position
    "EUCLID",    // Pattern state
    "TRIGGER",   // Trigger tracking
    "TIMELINE",  // Breakpoint automation
  ];

  for (const [name, _] of opcodes) {
    // Check prefixes
    for (const prefix of statefulPrefixes) {
      if (name.startsWith(prefix)) {
        stateful.add(name);
        break;
      }
    }
    // Check explicit list
    if (explicitStateful.includes(name)) {
      stateful.add(name);
    }
  }

  return stateful;
}

/**
 * Generate the header file content
 */
function generateHeader(opcodes: Map<string, number>, stateful: Set<string>): string {
  // Sort opcodes by value for consistent output
  const sortedOpcodes = Array.from(opcodes.entries()).sort((a, b) => a[1] - b[1]);

  // Generate opcode_to_string switch cases
  const toStringCases = sortedOpcodes
    .map(([name, _]) => `        case Opcode::${name}: return "${name}";`)
    .join("\n");

  // Generate opcode_is_stateful switch cases (only stateful ones)
  const statefulCases = sortedOpcodes
    .filter(([name, _]) => stateful.has(name))
    .map(([name, _]) => `        case Opcode::${name}:`)
    .join("\n");

  return `// AUTO-GENERATED FILE - DO NOT EDIT
// Generated by web/scripts/generate-opcode-metadata.ts
// To regenerate, run: cd web && bun run build:opcodes

#pragma once

#include <cedar/vm/instruction.hpp>

namespace cedar {

/**
 * Convert an opcode to its string representation
 */
inline const char* opcode_to_string(Opcode op) {
    switch (op) {
${toStringCases}
        default: return "UNKNOWN";
    }
}

/**
 * Check if an opcode requires state (has a state_id)
 * Stateful opcodes maintain internal state between blocks (oscillators, filters, delays, etc.)
 */
inline bool opcode_is_stateful(Opcode op) {
    switch (op) {
${statefulCases}
            return true;
        default:
            return false;
    }
}

} // namespace cedar
`;
}

function main() {
  console.log("Generating opcode metadata...");

  // Read source files
  console.log(`Reading ${INSTRUCTION_HPP}...`);
  const instructionSource = readFileSync(INSTRUCTION_HPP, "utf-8");

  console.log(`Reading ${BUILTINS_HPP}...`);
  const builtinsSource = readFileSync(BUILTINS_HPP, "utf-8");

  // Parse opcodes
  console.log("Parsing Opcode enum...");
  const opcodes = parseOpcodeEnum(instructionSource);
  console.log(`  Found ${opcodes.size} opcodes`);

  // Parse stateful opcodes from builtins
  console.log("Parsing stateful opcodes from builtins...");
  const fromBuiltins = parseStatefulOpcodes(builtinsSource);
  console.log(`  Found ${fromBuiltins.size} stateful opcodes from builtins`);

  // Infer additional stateful opcodes based on naming patterns
  console.log("Inferring additional stateful opcodes...");
  const stateful = inferStatefulOpcodes(opcodes, fromBuiltins);
  console.log(`  Total ${stateful.size} stateful opcodes after inference`);

  // Generate header
  console.log("Generating header...");
  const header = generateHeader(opcodes, stateful);

  // Ensure output directory exists
  if (!existsSync(OUTPUT_DIR)) {
    console.log(`Creating directory ${OUTPUT_DIR}...`);
    mkdirSync(OUTPUT_DIR, { recursive: true });
  }

  // Write output
  console.log(`Writing ${OUTPUT_FILE}...`);
  writeFileSync(OUTPUT_FILE, header);

  console.log("Done!");
  console.log(`\nGenerated ${OUTPUT_FILE}`);
  console.log(`  ${opcodes.size} opcodes`);
  console.log(`  ${stateful.size} stateful opcodes`);
}

main();
