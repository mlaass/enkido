#pragma once

#include <akkado/ast.hpp>

#include <cstdint>

namespace akkado {

/// Structural FNV-1a hash of an AST arena. Walks every node in arena
/// order and feeds the node's type, sibling links, and each `Node::data`
/// variant arm's named fields into the hash.
///
/// Intentionally field-wise (not memcpy) — `Node::data` holds
/// `std::string`s and `std::vector`s whose padding and heap pointers
/// are unspecified.
///
/// Used by `prd-parser-codegen-correctness.md` Phase 1a regression
/// tests (and, after Phase 1b, by a debug assertion in
/// `CodeGenerator::generate()` that codegen never mutates the input
/// AST).
[[nodiscard]] std::uint64_t arena_structural_hash(const AstArena& arena);

}  // namespace akkado
