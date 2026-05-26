#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace akkado {

/// Stable per-compile identifier for an interned string. Sequential
/// u32 assigned by `StringInterner::intern()` — equality compare via
/// `id == id` (no string compare, no rehash). 0 is reserved for
/// "empty / invalid".
using SymbolId = std::uint32_t;
constexpr SymbolId NULL_SYMBOL = 0;

/// Per-compile string interner. Owns its string storage (each unique
/// input is heap-allocated once) and assigns each a sequential
/// `SymbolId(u32)` so downstream consumers (Token, IdentifierData,
/// SymbolTable) can use fast id-equality instead of string-equality
/// and skip per-lookup FNV rehashing.
///
/// **Lifetime contract.** Interned `string_view`s returned by
/// `view(id)` reference the interner's own owned storage; they stay
/// valid for the interner's lifetime regardless of whether the
/// original source buffer is gone. This lets `CompileResult.symbols`
/// keep working after `compile()` returns. The original PRD design
/// called for view-into-source semantics; that was relaxed during
/// Phase 5 implementation when post-compile symbol-table inspection
/// proved to need the owned-string property.
///
/// **Concurrency.** v1 is single-threaded — no mutex. The API is
/// shaped so a future lock-free `intern()` (open-addressing CAS) can
/// drop in without changing call sites; that's left to a future PR
/// driven by per-import parallel lexing (audit F5 / PRD-3).
///
/// PRD prd-parser-codegen-correctness.md Phase 5 (F12).
class StringInterner {
public:
    StringInterner();

    /// Intern a string. Returns `NULL_SYMBOL` for empty input;
    /// otherwise either reuses the existing id for an identical string
    /// or assigns the next sequential id. The caller's `string_view`
    /// memory MUST outlive this interner.
    SymbolId intern(std::string_view sv);

    /// Look up an existing interned string without inserting. Returns
    /// `NULL_SYMBOL` if the string isn't already interned (or is
    /// empty). Useful for const-context table lookups — e.g.
    /// `SymbolTable::lookup(string_view)` where inserting an absent
    /// name would bloat the interner with non-symbol strings.
    [[nodiscard]] SymbolId find(std::string_view sv) const;

    /// O(1) resolve back to the originally-interned view. Calling
    /// with `NULL_SYMBOL` (or any id >= `size()`) returns an empty
    /// `string_view`.
    [[nodiscard]] std::string_view view(SymbolId id) const noexcept;

    /// O(1) cached FNV-1a 32-bit hash of the interned string (set at
    /// intern time, never recomputed). Returns 0 for `NULL_SYMBOL`.
    [[nodiscard]] std::uint32_t hash(SymbolId id) const noexcept;

    /// Number of unique strings interned, including the reserved
    /// NULL_SYMBOL slot at index 0. (So the smallest non-trivial size
    /// is 2: NULL_SYMBOL + one user string.)
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        std::string_view view;
        std::uint32_t hash;
    };

    std::vector<Entry> entries_;                                 // index = SymbolId
    std::unordered_map<std::string_view, SymbolId> by_view_;     // dedup by content
    // Heap-allocated owned storage (pointer-stable so views above
    // stay valid even when this vector grows).
    std::vector<std::unique_ptr<std::string>> owned_strings_;
};

} // namespace akkado
