#pragma once

#include "akkado/akkado.hpp"
#include "bytecode_loader.hpp"
#include "cedar/vm/vm.hpp"
#include "cedar/opcodes/dsp_state.hpp"

#include <iosfwd>
#include <vector>

namespace nkido {

// Resolve every URI-keyed asset (banks, soundfonts, single samples, wavetables)
// declared via `opts` (--bank/--soundfont/--sample) and via top-level akkado
// directives in `cr` (samples(), wt_load()). Errors and warnings go to
// `err`. Returns false only when an explicitly requested asset failed to load
// in a way that should abort the run.
bool prepare_program_assets(cedar::VM& vm,
                            const Options& opts,
                            const akkado::CompileResult& cr,
                            std::ostream& err);

// After samples are loaded into vm.sample_bank(), walk each
// SequenceProgram's sample mappings and write the resolved bank IDs back
// into the corresponding event values[] slots. Without this, events compiled
// without a SampleRegistry carry sample_id=0 and SAMPLE_PLAY produces
// silence even though the bank holds the audio. Mirrors the WASM build's
// `akkado_resolve_sample_ids`.
void resolve_sample_ids_in_events(cedar::VM& vm, akkado::CompileResult& cr);

// Apply SequenceProgram / PolyAlloc / Timeline state inits from the compile
// result. `seq_storage` holds the backing memory for every sequence's event
// array; it MUST outlive every subsequent process_block() call that could
// read those events. Hot-swap callers should append-only into a long-lived
// storage list and never reuse a slot.
void apply_state_inits(cedar::VM& vm,
                       const akkado::CompileResult& cr,
                       std::vector<std::vector<cedar::Sequence>>& seq_storage);

// Apply compile-time builtin variable overrides (e.g., `bpm = 140`) to the
// VM. Today this only handles `bpm` (via cedar::VM::set_bpm, which syncs
// ctx.bpm, the __bpm env param, and __spb). Other names are ignored — extend
// here when more settable builtins are added in akkado/builtins.hpp.
void apply_builtin_var_overrides(cedar::VM& vm,
                                 const akkado::CompileResult& cr);

// Used by non-hot-swap callers (render mode, the initial-load branches of
// play/serve/ui). Performs: assets → load_program_immediate → state inits.
//
// On a `.cedar` precompiled bytecode input (load.compile_result is nullopt)
// it skips asset resolution and state inits and only loads the bytecode; if
// any of --bank/--soundfont/--sample were also given, those CLI-flag assets
// are loaded against an empty CompileResult and a warning is emitted (the
// program's RequiredSample metadata is absent, so qualified lookups may
// still miss).
bool load_and_prepare_immediate(cedar::VM& vm,
                                const Options& opts,
                                LoadResult& load,
                                std::vector<std::vector<cedar::Sequence>>& seq_storage,
                                std::ostream& err);

}  // namespace nkido
