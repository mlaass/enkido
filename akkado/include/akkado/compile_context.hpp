#pragma once

#include <memory>

// PRD prd-parser-codegen-correctness.md Phase 4 (F14) + Phase 5 (F12).
//
// `CompileContext` is the per-compile-call state container that replaces
// the process-global `voicing_registry()` / `registry_mutex()` pair in
// `voicing.cpp` (Phase 4) and will hold the per-compile `StringInterner`
// (Phase 5).
//
// Default behaviour: `compile()` constructs a fresh `CompileContext` on
// the stack when the caller doesn't supply one. Callers that want
// cross-compile persistence (e.g. live-coding hosts that want to keep
// user-defined voicings across edits) own a `CompileContext` and pass it
// in to every `compile()` call.

namespace akkado {

namespace voicing { class VoicingRegistry; }

/// Per-compile state container. See PRD prd-parser-codegen-correctness.md
/// §3.1.
///
/// Lifetime contract (Phase 5 preview): once the interner is populated,
/// `CompileContext` MUST be destroyed before the source buffer it
/// interned views from. For single-call use this is automatic
/// (`compile()` owns both). Callers that reuse a `CompileContext` across
/// compiles must own persistent source storage.
struct CompileContext {
    std::unique_ptr<voicing::VoicingRegistry> voicing_registry;
    // Phase 5: std::unique_ptr<StringInterner> interner; goes here.

    CompileContext();
    ~CompileContext();

    CompileContext(const CompileContext&) = delete;
    CompileContext& operator=(const CompileContext&) = delete;
    CompileContext(CompileContext&&) noexcept;
    CompileContext& operator=(CompileContext&&) noexcept;
};

} // namespace akkado
