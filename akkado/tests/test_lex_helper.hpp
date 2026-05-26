#pragma once

// PRD prd-parser-codegen-correctness.md Phase 5 (F12):
//   public `akkado::lex(source, StringInterner&, filename)` and
//   public `akkado::parse(tokens, source, StringInterner&, filename, lint)`
// require an interner. This helper provides the pre-Phase-5 argument
// shapes (`lex(source)`, `parse(tokens, source)`) for tests that don't
// care about interner ownership — backed by a per-thread interner so
// SymbolIds stay resolvable across SECTION boundaries.
//
// Tests that DO care about interner identity (Phase 5 [F12] regression
// tests) should construct their own `StringInterner` and call the
// three-arg public API directly.

#include "akkado/lexer.hpp"
#include "akkado/parser.hpp"
#include "akkado/string_interner.hpp"

namespace akkado::test_helpers {

inline StringInterner& shared_test_interner() {
    static thread_local StringInterner i;
    return i;
}

inline auto lex(std::string_view source, std::string_view filename = "<input>") {
    return akkado::lex(source, shared_test_interner(), filename);
}

inline auto parse(std::vector<Token> tokens, std::string_view source,
                  std::string_view filename = "<input>", bool lint = false) {
    return akkado::parse(std::move(tokens), source, shared_test_interner(),
                         filename, lint);
}

inline std::string_view ident_text(SymbolId id) {
    return shared_test_interner().view(id);
}

} // namespace akkado::test_helpers
