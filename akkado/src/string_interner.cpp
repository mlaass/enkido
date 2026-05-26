#include "akkado/string_interner.hpp"

namespace akkado {

namespace {

// FNV-1a 32-bit. Mirrors the canonical declaration in
// akkado/symbol_table.hpp:51 so consumers that compare hashes across
// the two paths (e.g. cedar runtime param lookup) get identical bits.
inline std::uint32_t fnv1a_32(std::string_view sv) noexcept {
    std::uint32_t h = 2166136261u;
    for (char c : sv) {
        h ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        h *= 16777619u;
    }
    return h;
}

} // namespace

StringInterner::StringInterner() {
    // Slot 0 reserved for NULL_SYMBOL. Empty view + zero hash so
    // `view(NULL_SYMBOL)` returns {} and `hash(NULL_SYMBOL)` returns 0.
    entries_.push_back(Entry{std::string_view{}, 0u});
}

SymbolId StringInterner::intern(std::string_view sv) {
    if (sv.empty()) return NULL_SYMBOL;

    auto it = by_view_.find(sv);
    if (it != by_view_.end()) return it->second;

    const SymbolId id = static_cast<SymbolId>(entries_.size());
    const std::uint32_t h = fnv1a_32(sv);
    // PRD prd-parser-codegen-correctness.md Phase 5 (F12): the interner
    // owns its string storage. The header originally described
    // view-into-source semantics, but holding owned copies lets
    // `SymbolTable::lookup(string_view)` survive past the source
    // buffer's lifetime — required for `CompileResult.symbols` tooling
    // that inspects symbols after `compile()` returns.
    owned_strings_.push_back(std::make_unique<std::string>(sv));
    std::string_view stored{*owned_strings_.back()};
    entries_.push_back(Entry{stored, h});
    by_view_.emplace(stored, id);
    return id;
}

SymbolId StringInterner::find(std::string_view sv) const {
    if (sv.empty()) return NULL_SYMBOL;
    auto it = by_view_.find(sv);
    if (it == by_view_.end()) return NULL_SYMBOL;
    return it->second;
}

std::string_view StringInterner::view(SymbolId id) const noexcept {
    if (id >= entries_.size()) return std::string_view{};
    return entries_[id].view;
}

std::uint32_t StringInterner::hash(SymbolId id) const noexcept {
    if (id >= entries_.size()) return 0u;
    return entries_[id].hash;
}

} // namespace akkado
