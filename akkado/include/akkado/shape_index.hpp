#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace akkado {

struct CompileArtifacts;  // akkado/akkado.hpp

/// Sentinel passed for `cursor_offset` to disable patternHole resolution.
constexpr std::uint32_t SHAPE_INDEX_NO_CURSOR = 0xFFFFFFFFu;

/// FNV-1a 32-bit over the raw source bytes — identical to the editor's
/// JS-side `fnv1a32` cache key. compile() stashes this on
/// CompileArtifacts::user_source_hash; the shape JSON surfaces it as
/// "sourceHash".
std::uint32_t shape_index_source_hash(std::string_view src);

/// Serialize the JSON shape index for editor autocomplete (Phase 2 of
/// records-system-unification PRD; rewritten over CompileArtifacts by
/// hardening Phase 8 / PRD-2 — no re-lex/re-parse, reuses the main
/// compile's parse tree).
///
/// Walks `artifacts.parsed_ast`'s top-level assignments within the
/// user-source region and emits per-binding shape records for every
/// `Record`, `Pattern`, or `Array`-of-`Record` binding. Pattern shapes
/// include the 11 fixed PatternPayload slots, every alias from
/// `pattern_field_aliases()`, and source-derived `custom_fields` from
/// `.set()` chains (collision dedupe — fixed wins per PRD §10.5).
///
/// `cursor_offset` is a UTF-8 byte offset in the *user* source (the
/// editor buffer); it is rebased onto the combined source internally.
/// When the cursor sits inside a `Pipe` whose LHS resolves to a
/// Pattern-typed top-level binding, the result includes a top-level
/// `patternHole` entry with that pattern's shape. Pass
/// SHAPE_INDEX_NO_CURSOR to skip the resolution entirely.
///
/// Tolerant of failed compiles — compile() populates `parsed_ast` on
/// parse-error paths too, so bindings that parsed before the first
/// error still surface. Never throws.
std::string serialize_shape_index(const CompileArtifacts& artifacts,
                                  std::uint32_t cursor_offset = SHAPE_INDEX_NO_CURSOR);

}  // namespace akkado
