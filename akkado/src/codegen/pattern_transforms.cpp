// Pattern-transform call handlers (bank/variant/transport/tune/...).
// Phase 5 consolidated the SequenceProgram-recompiling handlers onto
// emit_seqpat_transform (bank/variant/tune/anchor/mode) and the runtime
// structural transforms onto emit_reorder_call / emit_fanout_call.
// Extracted verbatim from patterns.cpp (prd-codegen-sprawl-cleanup Phase 3).

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/codegen/instruction_builder.hpp"
#include "akkado/codegen/state_init_builder.hpp"
#include "akkado/codegen/options.hpp"
#include "akkado/compile_context.hpp"
#include "akkado/chord_parser.hpp"
#include "akkado/pattern_eval.hpp"
#include "akkado/mini_parser.hpp"
#include "akkado/pattern_debug.hpp"
#include "akkado/tuning.hpp"
#include "akkado/voicing.hpp"
#include <cedar/opcodes/sequence.hpp>
#include <cedar/opcodes/event_transform_encoding.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include "pattern_compiler.hpp"

namespace akkado {

using codegen::SamplePatternEmitCtx;
using codegen::emit_sample_chain;
using codegen::sample_refs_from_mappings;
using codegen::mini_content_location;
using codegen::get_pattern_arg;
using codegen::get_number_arg;
using codegen::get_string_arg;
using codegen::is_pattern_node;

// ============================================================================
// Pattern transformation handlers
// ============================================================================

// Helpers: synthesize events for Phase 2 PRD generators (run/binary/binaryN).
// Each returns the event count for cycle_length sizing; caller stores events
// either directly into the compiler or into out_events for transform recursion.
static std::vector<cedar::Event> synth_run_events(int n) {
    std::vector<cedar::Event> events;
    if (n <= 0) return events;
    events.reserve(static_cast<std::size_t>(n));
    float step = 1.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        cedar::Event e;
        e.type = cedar::EventType::DATA;
        e.time = static_cast<float>(i) * step;
        e.duration = step;
        e.num_values = 1;
        e.values[0] = static_cast<float>(i);
        e.velocity = 1.0f;
        e.chance = 1.0f;
        events.push_back(e);
    }
    return events;
}

static std::vector<cedar::Event> synth_binary_events(std::uint32_t n, int bits) {
    // Emit `bits` events MSB-first; set bits emit triggers (num_values=1,
    // value=1.0), unset bits emit rests (num_values=0). Per PRD §5.3.
    std::vector<cedar::Event> events;
    if (bits <= 0) return events;
    events.reserve(static_cast<std::size_t>(bits));
    float step = 1.0f / static_cast<float>(bits);
    for (int i = 0; i < bits; ++i) {
        int bit_pos = bits - 1 - i;  // MSB first
        bool is_set = (n >> bit_pos) & 1u;
        cedar::Event e;
        e.type = cedar::EventType::DATA;
        e.time = static_cast<float>(i) * step;
        e.duration = step;
        e.velocity = 1.0f;
        e.chance = 1.0f;
        if (is_set) {
            e.num_values = 1;
            e.values[0] = 1.0f;
        } else {
            e.num_values = 0;  // rest
        }
        events.push_back(e);
    }
    return events;
}

static int compute_binary_bits(std::uint32_t n) {
    if (n == 0) return 1;
    int bits = 0;
    std::uint32_t v = n;
    while (v > 0) { ++bits; v >>= 1; }
    return bits;
}

// Helper: Compile a pattern and return the compiled data
// Returns true on success. On success, populates out_* parameters.
// out_events and out_cycle_length carry already-transformed events from inner transforms.
// PRD Phase 1b: `out_arena` reports which AST arena `out_pattern_node` lives
// in — MiniLiteral leaves come from a sub-arena (MiniLiteralData::mini_arena),
// codegen-time re-parses from a codegen scratch arena, and transform/generator
// branches still point into the main analyzer AST arena.
static bool compile_pattern_for_transform(
    CodeGenerator& gen,
    const Ast& ast,
    NodeIndex pattern_arg,
    SampleRegistry* sample_registry,
    SequenceCompiler& compiler,
    NodeIndex& out_pattern_node,
    const AstArena*& out_arena,
    std::uint32_t& out_num_elements,
    std::vector<std::vector<cedar::Event>>& out_events,
    float& out_cycle_length) {

    const Node& pat_node = ast.arena[pattern_arg];

    // Case 1: MiniLiteral (base case)
    if (pat_node.type == NodeType::MiniLiteral) {
        // PRD Phase 1b: the parsed mini-AST lives in MiniLiteralData's
        // sub-arena, not as a first-child of the MiniLiteral node.
        const auto& lit_data = pat_node.as_mini_literal();
        if (!lit_data.mini_arena || lit_data.mini_root == NULL_NODE) {
            return false;
        }
        const AstArena& mini_arena = *lit_data.mini_arena;
        out_pattern_node = lit_data.mini_root;
        out_arena = &mini_arena;

        const Node& pattern = mini_arena[out_pattern_node];
        compiler.set_arena(mini_arena);
        compiler.set_pattern_base_offset(pattern.location.offset);

        if (!compiler.compile(out_pattern_node)) {
            return false;
        }

        out_num_elements = compiler.count_top_level_elements(out_pattern_node);
        out_events = compiler.sequence_events();
        out_cycle_length = compiler.cycle_length();
        return true;
    }

    // Case 1b: StringLit — parse as mini-notation into a codegen scratch arena.
    // PRD Phase 1b: replaces the legacy const_cast<AstArena&>(ast.arena).
    if (pat_node.type == NodeType::StringLit) {
        std::string pattern_str = pat_node.as_string();
        AstArena& scratch = gen.acquire_mini_scratch_arena();
        auto [pattern_root, diags] = parse_mini(pattern_str, scratch,
            mini_content_location(pat_node.location), false);
        if (pattern_root == NULL_NODE) return false;

        out_pattern_node = pattern_root;
        out_arena = &scratch;
        const Node& pattern = scratch[out_pattern_node];
        compiler.set_arena(scratch);
        compiler.set_pattern_base_offset(pattern.location.offset);

        if (!compiler.compile(out_pattern_node)) return false;

        out_num_elements = compiler.count_top_level_elements(out_pattern_node);
        out_events = compiler.sequence_events();
        out_cycle_length = compiler.cycle_length();
        return true;
    }

    // Case 1c: Identifier bound to a Pattern — recurse on the bound AST node.
    // The analyzer records the originating MiniLiteral / pat-call node in
    // PatternInfo.pattern_node when it sees `name = n"…"` or `name = pat(…)`,
    // so we follow the binding back to its source. Without this case,
    // `melody = n"…"; transpose(melody, …)` (or its pipe form `melody |>
    // transpose(@, …)`) fails E130 even though `is_pattern_node()` accepts
    // the identifier as a valid pattern argument.
    if (pat_node.type == NodeType::Identifier) {
        auto sym = gen.symbols().lookup(pat_node.as_identifier());
        if (!sym || sym->kind != SymbolKind::Pattern) return false;
        NodeIndex bound = sym->pattern.pattern_node;
        if (bound == NULL_NODE || bound == pattern_arg) return false;
        return compile_pattern_for_transform(gen, ast, bound, sample_registry,
                                             compiler, out_pattern_node, out_arena,
                                             out_num_elements,
                                             out_events, out_cycle_length);
    }

    // Handle Call nodes
    if (pat_node.type == NodeType::Call) {
        std::string func_name = std::string(gen.ctx().interner->view(pat_node.as_identifier()));

        // Case 2.4: chord(...) base case — parse chord string and compile
        // through the same mini-notation pipeline used by handle_chord_call.
        // PRD Phase 1b: parse into a codegen scratch arena.
        if (func_name == "chord") {
            NodeIndex first_arg = pat_node.first_child;
            if (first_arg == NULL_NODE) return false;
            const Node& arg_node = ast.arena[first_arg];
            NodeIndex str_node = (arg_node.type == NodeType::Argument)
                ? arg_node.first_child : first_arg;
            if (str_node == NULL_NODE) return false;
            const Node& str_n = ast.arena[str_node];
            if (str_n.type != NodeType::StringLit) return false;
            std::string chord_str = str_n.as_string();
            AstArena& scratch = gen.acquire_mini_scratch_arena();
            auto [pattern_root, diags] = parse_mini(chord_str, scratch,
                mini_content_location(str_n.location), /*sample_only=*/false);
            if (pattern_root == NULL_NODE) return false;
            out_pattern_node = pattern_root;
            out_arena = &scratch;
            const Node& pattern = scratch[out_pattern_node];
            compiler.set_arena(scratch);
            compiler.set_pattern_base_offset(pattern.location.offset);
            if (!compiler.compile(out_pattern_node)) return false;
            out_num_elements = compiler.count_top_level_elements(out_pattern_node);
            out_events = compiler.sequence_events();
            out_cycle_length = compiler.cycle_length();
            return true;
        }

        // Case 2.5: Generator constructors (run/binary/binaryN) — synthesize
        // events directly into the compiler and seed out_* like a base case.
        if (func_name == "run") {
            auto n_arg = get_number_arg(ast, pat_node, 0);
            if (!n_arg.has_value() || *n_arg < 0) return false;
            int n_int = static_cast<int>(*n_arg);
            auto events = synth_run_events(n_int);
            compiler.populate_synthetic(std::move(events));
            out_pattern_node = pat_node.first_child;
            out_arena = &ast.arena;
            out_num_elements = static_cast<std::uint32_t>(std::max(1, n_int));
            out_events = compiler.sequence_events();
            out_cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)
            return true;
        }
        if (func_name == "binary") {
            auto n_arg = get_number_arg(ast, pat_node, 0);
            if (!n_arg.has_value() || *n_arg < 0) return false;
            std::uint32_t n_val = static_cast<std::uint32_t>(*n_arg);
            int bits = compute_binary_bits(n_val);
            auto events = synth_binary_events(n_val, bits);
            compiler.populate_synthetic(std::move(events));
            out_pattern_node = pat_node.first_child;
            out_arena = &ast.arena;
            out_num_elements = static_cast<std::uint32_t>(std::max(1, bits));
            out_events = compiler.sequence_events();
            out_cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)
            return true;
        }
        if (func_name == "binaryN") {
            auto n_arg = get_number_arg(ast, pat_node, 0);
            auto bits_arg = get_number_arg(ast, pat_node, 1);
            if (!n_arg.has_value() || !bits_arg.has_value()) return false;
            if (*n_arg < 0 || *bits_arg < 0) return false;
            int bits = static_cast<int>(*bits_arg);
            if (bits < 0) return false;
            std::uint32_t n_val = static_cast<std::uint32_t>(*n_arg);
            // Truncate to lower `bits` bits per PRD §9.2.
            if (bits < 32) {
                n_val &= (1u << bits) - 1u;
            }
            auto events = synth_binary_events(n_val, bits);
            compiler.populate_synthetic(std::move(events));
            out_pattern_node = pat_node.first_child;
            out_arena = &ast.arena;
            out_num_elements = static_cast<std::uint32_t>(std::max(1, bits));
            out_events = compiler.sequence_events();
            out_cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)
            return true;
        }

        // Case 3: Transform calls (recursive case)
        bool is_transform = (func_name == "slow" || func_name == "fast" ||
                             func_name == "rev" || func_name == "transpose" ||
                             func_name == "velocity" || func_name == "bank" ||
                             func_name == "variant" || func_name == "tune" ||
                             // Phase 2 PRD time/structure transforms
                             func_name == "early" || func_name == "late" ||
                             func_name == "palindrome" || func_name == "compress" ||
                             func_name == "ply" || func_name == "linger" ||
                             func_name == "zoom" || func_name == "segment" ||
                             func_name == "swing" || func_name == "swingBy" ||
                             func_name == "iter" || func_name == "iterBack" ||
                             // Phase 2 PRD voicing transforms (state-only;
                             // top-level handler applies voice_chords)
                             func_name == "anchor" || func_name == "mode" ||
                             func_name == "voicing" ||
                             // Phase 2.1 PRD §11.2: note-property transforms
                             func_name == "bend" || func_name == "aftertouch" ||
                             func_name == "dur");
        if (is_transform) {
            // tune() sets context BEFORE compilation (not post-processing)
            if (func_name == "tune") {
                auto tuning_name = get_string_arg(ast, pat_node, 0);
                NodeIndex inner_arg = get_pattern_arg(ast, pat_node, 1);
                if (!tuning_name.has_value() || inner_arg == NULL_NODE) return false;

                auto tuning = parse_tuning(*tuning_name);
                if (!tuning.has_value()) return false;

                compiler.set_tuning(*tuning);
                return compile_pattern_for_transform(gen, ast, inner_arg, sample_registry,
                                                      compiler, out_pattern_node, out_arena,
                                                      out_num_elements,
                                                      out_events, out_cycle_length);
            }

            // Get the inner pattern argument (first arg of this transform call)
            NodeIndex inner_arg = get_pattern_arg(ast, pat_node, 0);
            if (inner_arg == NULL_NODE) return false;

            // Recursively compile the inner pattern
            if (!compile_pattern_for_transform(gen, ast, inner_arg, sample_registry,
                                                compiler, out_pattern_node, out_arena,
                                                out_num_elements,
                                                out_events, out_cycle_length)) {
                return false;
            }

            // Apply this transform to the already-compiled events
            if (func_name == "slow") {
                auto factor = get_number_arg(ast, pat_node, 1);
                if (!factor.has_value() || *factor <= 0) return false;
                // Only scale cycle_length — event times are normalized [0,1)
                out_cycle_length *= *factor;
            } else if (func_name == "fast") {
                auto factor = get_number_arg(ast, pat_node, 1);
                if (!factor.has_value() || *factor <= 0) return false;
                // Only scale cycle_length — event times are normalized [0,1)
                out_cycle_length /= *factor;
            } else if (func_name == "rev") {
                for (auto& seq_events : out_events) {
                    for (auto& event : seq_events) {
                        float new_time = 1.0f - event.time - event.duration;
                        if (new_time < 0.0f) new_time = 0.0f;
                        event.time = new_time;
                    }
                }
            } else if (func_name == "transpose") {
                auto semitones = get_number_arg(ast, pat_node, 1);
                if (!semitones.has_value()) return false;
                float ratio = std::pow(2.0f, *semitones / 12.0f);
                const float st = static_cast<float>(*semitones);
                if (!compiler.is_sample_pattern()) {
                    for (auto& seq_events : out_events) {
                        for (auto& event : seq_events) {
                            // Shift frequencies *and* the MIDI note fields so
                            // %.note / per-voice .note track the transposition.
                            if (event.type == cedar::EventType::DATA &&
                                event.num_values > 0) {
                                for (std::uint8_t i = 0; i < event.num_values; ++i) {
                                    event.values[i] *= ratio;
                                    event.notes[i] += st;
                                }
                                event.midi_note += st;
                            }
                        }
                    }
                }
            } else if (func_name == "velocity") {
                auto vel = get_number_arg(ast, pat_node, 1);
                if (!vel.has_value() || *vel < 0 || *vel > 1) return false;
                for (auto& seq_events : out_events) {
                    for (auto& event : seq_events) {
                        event.velocity *= *vel;
                    }
                }
            } else if (func_name == "bend" || func_name == "aftertouch") {
                // Phase 2.1 PRD §11.2: standalone bend/aftertouch transforms.
                // Reserve a slot keyed by the transform name (so users can
                // access via e.bend / e.aftertouch after pipe-binding) and
                // write the value to event.prop_vals[slot] on every event.
                auto value = get_number_arg(ast, pat_node, 1);
                if (!value.has_value()) return false;
                int slot = compiler.allocate_property_slot(func_name);
                if (slot < 0) return false;  // overflow — drop the transform
                for (auto& seq_events : out_events) {
                    for (auto& event : seq_events) {
                        event.prop_vals[slot] = *value;
                        event.prop_set_mask |=
                            static_cast<std::uint8_t>(1u << slot);
                    }
                }
            } else if (func_name == "dur") {
                // Phase 2.1 PRD §11.2: pure compile-time duration multiplier.
                // event.duration already exists on cedar::Event, so no slot
                // plumbing needed.
                auto factor = get_number_arg(ast, pat_node, 1);
                if (!factor.has_value() || *factor <= 0) return false;
                for (auto& seq_events : out_events) {
                    for (auto& event : seq_events) {
                        event.duration *= *factor;
                    }
                }
            } else if (func_name == "bank") {
                auto bank_name = get_string_arg(ast, pat_node, 1);
                if (!bank_name.has_value()) return false;
                for (auto& mapping : compiler.mutable_sample_mappings()) {
                    mapping.bank = *bank_name;
                }
            } else if (func_name == "variant") {
                auto variant = get_number_arg(ast, pat_node, 1);
                if (!variant.has_value() || *variant < 0) return false;
                for (auto& mapping : compiler.mutable_sample_mappings()) {
                    mapping.variant = static_cast<std::uint8_t>(*variant);
                }
            } else if (func_name == "early") {
                // early(pat, n): t' = (t - n + 1) mod 1; wraps within [0,1).
                // Only shift root sequence events. Sub-seq events have local
                // time relative to their parent slot; shifting them double-applies
                // when the parent NORMAL evaluator re-adds e.time.
                auto amount = get_number_arg(ast, pat_node, 1);
                if (!amount.has_value()) return false;
                float a = std::fmod(*amount, 1.0f);
                if (a < 0.0f) a += 1.0f;
                if (!out_events.empty()) {
                    auto& seq_events = out_events[0];
                    for (auto& event : seq_events) {
                        float t = event.time - a;
                        t = std::fmod(t, 1.0f);
                        if (t < 0.0f) t += 1.0f;
                        event.time = t;
                    }
                    std::sort(seq_events.begin(), seq_events.end(),
                        [](const cedar::Event& x, const cedar::Event& y) {
                            return x.time < y.time;
                        });
                }
            } else if (func_name == "late") {
                // late(pat, n): t' = (t + n) mod 1; wraps within [0,1).
                // Only shift root sequence events (see early() above for why).
                auto amount = get_number_arg(ast, pat_node, 1);
                if (!amount.has_value()) return false;
                float a = std::fmod(*amount, 1.0f);
                if (a < 0.0f) a += 1.0f;
                if (!out_events.empty()) {
                    auto& seq_events = out_events[0];
                    for (auto& event : seq_events) {
                        float t = event.time + a;
                        t = std::fmod(t, 1.0f);
                        if (t < 0.0f) t += 1.0f;
                        event.time = t;
                    }
                    std::sort(seq_events.begin(), seq_events.end(),
                        [](const cedar::Event& x, const cedar::Event& y) {
                            return x.time < y.time;
                        });
                }
            } else if (func_name == "palindrome") {
                // palindrome(pat): forward in [0, 0.5), reversed in [0.5, 1);
                // doubles cycle_length so the full forward+reverse takes 2x.
                for (auto& seq_events : out_events) {
                    std::vector<cedar::Event> doubled;
                    doubled.reserve(seq_events.size() * 2);
                    for (const auto& event : seq_events) {
                        cedar::Event e = event;
                        e.time = event.time * 0.5f;
                        e.duration = event.duration * 0.5f;
                        doubled.push_back(e);
                    }
                    for (const auto& event : seq_events) {
                        cedar::Event e = event;
                        e.time = 1.0f - 0.5f * event.time - 0.5f * event.duration;
                        e.duration = event.duration * 0.5f;
                        if (e.time < 0.0f) e.time = 0.0f;
                        doubled.push_back(e);
                    }
                    std::sort(doubled.begin(), doubled.end(),
                        [](const cedar::Event& x, const cedar::Event& y) {
                            return x.time < y.time;
                        });
                    seq_events = std::move(doubled);
                }
                out_cycle_length *= 2.0f;
            } else if (func_name == "compress") {
                // compress(pat, s, e): squash all events into [s, e) of cycle.
                auto start_arg = get_number_arg(ast, pat_node, 1);
                auto end_arg = get_number_arg(ast, pat_node, 2);
                if (!start_arg.has_value() || !end_arg.has_value()) return false;
                float s = *start_arg;
                float e = *end_arg;
                if (e <= s) return false;
                float width = e - s;
                for (auto& seq_events : out_events) {
                    for (auto& event : seq_events) {
                        event.time = s + event.time * width;
                        event.duration *= width;
                    }
                }
            } else if (func_name == "ply") {
                // ply(pat, n): replace each event of duration d at time t with n
                // events of duration d/n at times t, t+d/n, ..., t+(n-1)*d/n.
                auto n_arg = get_number_arg(ast, pat_node, 1);
                if (!n_arg.has_value() || *n_arg < 1) return false;
                int n = static_cast<int>(*n_arg);
                if (n < 1) return false;
                for (auto& seq_events : out_events) {
                    std::vector<cedar::Event> plied;
                    plied.reserve(seq_events.size() * static_cast<std::size_t>(n));
                    for (const auto& ev : seq_events) {
                        float sub_d = ev.duration / static_cast<float>(n);
                        for (int i = 0; i < n; ++i) {
                            cedar::Event pe = ev;
                            pe.duration = sub_d;
                            pe.time = ev.time + static_cast<float>(i) * sub_d;
                            plied.push_back(pe);
                        }
                    }
                    seq_events = std::move(plied);
                }
            } else if (func_name == "linger") {
                // linger(pat, frac): equivalent to zoom(0, frac).fast(1/frac).
                // Drops events with time >= frac, scales remaining by 1/frac so
                // they fill [0, 1), then divides cycle_length by 1/frac (= *frac)
                // so the truncated portion repeats 1/frac times per logical cycle.
                auto frac_arg = get_number_arg(ast, pat_node, 1);
                if (!frac_arg.has_value() || *frac_arg <= 0) return false;
                float frac = *frac_arg;
                if (frac < 1.0f) {
                    for (auto& seq_events : out_events) {
                        std::vector<cedar::Event> kept;
                        for (const auto& ev : seq_events) {
                            if (ev.time < frac) {
                                cedar::Event ke = ev;
                                ke.time = ev.time / frac;
                                ke.duration = ev.duration / frac;
                                if (ke.time >= 1.0f) continue;
                                if (ke.time + ke.duration > 1.0f) {
                                    ke.duration = 1.0f - ke.time;
                                }
                                kept.push_back(ke);
                            }
                        }
                        seq_events = std::move(kept);
                    }
                    out_cycle_length *= frac;
                }
                // frac >= 1: no-op (keeps full pattern)
            } else if (func_name == "zoom") {
                // zoom(pat, s, e): keep events overlapping [s, e), remap to [0, 1).
                auto start_arg = get_number_arg(ast, pat_node, 1);
                auto end_arg = get_number_arg(ast, pat_node, 2);
                if (!start_arg.has_value() || !end_arg.has_value()) return false;
                float s = *start_arg;
                float e_val = *end_arg;
                if (e_val <= s) return false;
                float width = e_val - s;
                for (auto& seq_events : out_events) {
                    std::vector<cedar::Event> kept;
                    for (const auto& ev : seq_events) {
                        float event_end = ev.time + ev.duration;
                        if (event_end <= s || ev.time >= e_val) continue;
                        cedar::Event ke = ev;
                        float clipped_start = std::max(ev.time, s);
                        float clipped_end = std::min(event_end, e_val);
                        ke.time = (clipped_start - s) / width;
                        ke.duration = (clipped_end - clipped_start) / width;
                        kept.push_back(ke);
                    }
                    seq_events = std::move(kept);
                }
            } else if (func_name == "iter" || func_name == "iterBack") {
                // iter/iterBack configure runtime rotation state, not events.
                // In nested context (recursive case), inner pattern is already
                // compiled. The outer transform's handler will emit state
                // without iter set, so iter/iterBack are only effective when
                // applied at the outermost transform level. Documented in §5.2.
            } else if (func_name == "anchor") {
                // Voicing transform — accumulate state only. The outermost
                // voicing transform's handler calls apply_voicing.
                auto anchor_str = get_string_arg(ast, pat_node, 1);
                if (anchor_str.has_value()) {
                    auto midi = voicing::parse_anchor(*anchor_str);
                    if (midi.has_value()) compiler.set_voicing_anchor(*midi);
                }
            } else if (func_name == "mode") {
                auto mode_str = get_string_arg(ast, pat_node, 1);
                if (mode_str.has_value()) {
                    auto m = voicing::parse_mode(*mode_str);
                    if (m.has_value()) compiler.set_voicing_mode(*m);
                }
            } else if (func_name == "voicing") {
                auto name_str = get_string_arg(ast, pat_node, 1);
                if (name_str.has_value()) compiler.set_voicing_dict(*name_str);
            } else if (func_name == "swing" || func_name == "swingBy") {
                // swing(pat, n=4) ≡ swingBy(pat, 1/3, n=4).
                // swingBy(pat, amount, n=4): divide cycle into n slices; events
                // whose offset within their slice is >= 0.5 get time +=
                // amount * (1/(2n)).
                float amount;
                int n_slices;
                if (func_name == "swing") {
                    amount = 1.0f / 3.0f;
                    auto n_arg = get_number_arg(ast, pat_node, 1);
                    n_slices = n_arg.has_value() ? static_cast<int>(*n_arg) : 4;
                } else {
                    auto amt_arg = get_number_arg(ast, pat_node, 1);
                    if (!amt_arg.has_value()) return false;
                    amount = *amt_arg;
                    auto n_arg = get_number_arg(ast, pat_node, 2);
                    n_slices = n_arg.has_value() ? static_cast<int>(*n_arg) : 4;
                }
                if (n_slices < 1) return false;
                float slice_w = 1.0f / static_cast<float>(n_slices);
                float shift = amount * (slice_w * 0.5f);
                for (auto& seq_events : out_events) {
                    for (auto& event : seq_events) {
                        float slice_pos = event.time / slice_w;
                        float frac = slice_pos - std::floor(slice_pos);
                        if (frac >= 0.5f) {
                            event.time += shift;
                            if (event.time >= 1.0f) event.time -= 1.0f;
                        }
                    }
                    std::sort(seq_events.begin(), seq_events.end(),
                        [](const cedar::Event& x, const cedar::Event& y) {
                            return x.time < y.time;
                        });
                }
            } else if (func_name == "segment") {
                // segment(pat, n): sample at n evenly-spaced points; emit one
                // event per sample carrying the active event's value.
                auto n_arg = get_number_arg(ast, pat_node, 1);
                if (!n_arg.has_value() || *n_arg < 1) return false;
                int n = static_cast<int>(*n_arg);
                if (n < 1) return false;
                float step = 1.0f / static_cast<float>(n);
                for (auto& seq_events : out_events) {
                    if (seq_events.empty()) continue;
                    std::sort(seq_events.begin(), seq_events.end(),
                        [](const cedar::Event& x, const cedar::Event& y) {
                            return x.time < y.time;
                        });
                    std::vector<cedar::Event> sampled;
                    sampled.reserve(static_cast<std::size_t>(n));
                    for (int i = 0; i < n; ++i) {
                        float st = static_cast<float>(i) * step;
                        const cedar::Event* active = nullptr;
                        for (const auto& ev : seq_events) {
                            if (ev.time <= st && (ev.time + ev.duration) > st) {
                                active = &ev;
                            }
                        }
                        if (active == nullptr) {
                            // Fall back to last event whose start is <= st;
                            // else first event in the list.
                            for (const auto& ev : seq_events) {
                                if (ev.time <= st) active = &ev;
                            }
                            if (active == nullptr) active = &seq_events.front();
                        }
                        cedar::Event se = *active;
                        se.time = st;
                        se.duration = step;
                        sampled.push_back(se);
                    }
                    seq_events = std::move(sampled);
                }
            }
            return true;
        }

        // Case 2: pat/seq/timeline calls (base case — unwrap to MiniLiteral)
        NodeIndex first_arg = pat_node.first_child;
        if (first_arg != NULL_NODE) {
            const Node& arg_node = ast.arena[first_arg];
            NodeIndex actual_arg = first_arg;

            // Unwrap Argument node
            if (arg_node.type == NodeType::Argument) {
                actual_arg = arg_node.first_child;
            }

            if (actual_arg != NULL_NODE) {
                const Node& actual_node = ast.arena[actual_arg];
                if (actual_node.type == NodeType::MiniLiteral) {
                    // PRD Phase 1b: mini-AST lives in MiniLiteralData's sub-arena.
                    const auto& lit_data = actual_node.as_mini_literal();
                    if (lit_data.mini_arena && lit_data.mini_root != NULL_NODE) {
                        const AstArena& mini_arena = *lit_data.mini_arena;
                        out_pattern_node = lit_data.mini_root;
                        out_arena = &mini_arena;
                        const Node& pattern = mini_arena[out_pattern_node];
                        compiler.set_arena(mini_arena);
                        compiler.set_pattern_base_offset(pattern.location.offset);

                        if (compiler.compile(out_pattern_node)) {
                            out_num_elements = compiler.count_top_level_elements(out_pattern_node);
                            out_events = compiler.sequence_events();
                            out_cycle_length = compiler.cycle_length();
                            return true;
                        }
                    }
                }
            }
        }
    }

    return false;
}

// Helper: Emit a compiled pattern with transformations applied
// This is the common code for emitting SEQPAT_QUERY/SEQPAT_STEP
static TypedValue emit_pattern_with_state(
    CodeGenerator& gen,
    BufferAllocator& buffers,
    std::vector<StateInitData>& state_inits,
    std::set<std::string>& required_samples,
    std::unordered_map<NodeIndex, TypedValue>& node_types,
    NodeIndex node,
    std::uint32_t state_id,
    float cycle_length,
    const SequenceCompiler& compiler,
    std::vector<std::vector<cedar::Event>>& sequence_events,
    const SourceLocation& pattern_loc,
    const SourceLocation& call_loc) {

    // Collect required samples
    compiler.collect_samples(required_samples);

    bool is_sample_pattern = compiler.is_sample_pattern();

    // Allocate buffers for outputs
    std::uint16_t value_buf = buffers.allocate();
    std::uint16_t velocity_buf = buffers.allocate();
    std::uint16_t trigger_buf = buffers.allocate();

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    // Emit SEQPAT_QUERY instruction
    codegen::InstructionBuilder(cedar::Opcode::SEQPAT_QUERY)
        .output(0xFFFF)
        .state_id(state_id)
        .emit(gen);

    // Check for polyphonic patterns. Compute from the local (possibly
    // post-voicing) sequence_events rather than compiler.max_voices(), since
    // apply_voicing rewrites events.num_values on the local copy without
    // touching the compiler's internal state. Reading from compiler would
    // give stale parser-time voice counts (e.g. 3 for a CM triad) even when
    // voicing has expanded the chord to 5 voices.
    std::uint8_t max_voices = 1;
    for (const auto& seq : sequence_events) {
        for (const auto& e : seq) {
            if (e.num_values > max_voices) max_voices = e.num_values;
        }
    }
    std::vector<std::uint16_t> voice_buffers;

    // Emit SEQPAT_STEP for each voice
    for (std::uint8_t voice = 0; voice < max_voices; ++voice) {
        std::uint16_t voice_value_buf = (voice == 0) ? value_buf : buffers.allocate();
        if (voice_value_buf == BufferAllocator::BUFFER_UNUSED) {
            return TypedValue::void_val();
        }

        codegen::InstructionBuilder(cedar::Opcode::SEQPAT_STEP)
            .input(0, (voice == 0) ? velocity_buf : 0xFFFF)
            .input(1, (voice == 0) ? trigger_buf : 0xFFFF)
            .input(2, voice)
            .output(voice_value_buf)
            .state_id(state_id)
            .emit(gen);

        voice_buffers.push_back(voice_value_buf);
    }

    // Store sequence program initialization data
    StateInitData seq_init;
    seq_init.state_id = state_id;
    seq_init.type = StateInitData::Type::SequenceProgram;
    seq_init.cycle_length = cycle_length;
    seq_init.sequences = compiler.sequences();
    seq_init.sequence_events = std::move(sequence_events);
    seq_init.total_events = compiler.total_events();
    seq_init.is_sample_pattern = is_sample_pattern;
    seq_init.pattern_location = pattern_loc;
    seq_init.sequence_sample_mappings = compiler.sample_mappings();
    state_inits.push_back(std::move(seq_init));

    std::uint16_t result_buf = value_buf;

    // All SAMPLE_PLAY emission goes through emit_sample_chain. See
    // docs/prd-sample-emission-unification.md.
    if (is_sample_pattern) {
        SamplePatternEmitCtx ctx;
        ctx.kind = SamplePatternEmitCtx::Kind::Pattern;
        ctx.seq_state_id = state_id;
        ctx.value_buf = value_buf;
        ctx.trigger_buf = trigger_buf;
        ctx.velocity_buf = velocity_buf;
        ctx.loc = call_loc;
        std::uint16_t output_buf = emit_sample_chain(
            buffers,
            [&gen](const cedar::Instruction& i) { gen.emit(i); },
            ctx);
        if (output_buf == BufferAllocator::BUFFER_UNUSED) {
            return TypedValue::void_val();
        }
        result_buf = output_buf;
    }

    // Build PatternPayload
    auto payload = std::make_shared<PatternPayload>();
    payload->fields[PatternPayload::FREQ] = value_buf;
    payload->fields[PatternPayload::VEL] = velocity_buf;
    payload->fields[PatternPayload::TRIG] = trigger_buf;
    payload->state_id = state_id;
    payload->cycle_length = cycle_length;
    payload->is_sample_pattern = is_sample_pattern;
    payload->max_voices = max_voices;
    if (max_voices > 1 && !is_sample_pattern) {
        payload->voice_freqs = std::move(voice_buffers);
    }

    // Records-and-field-access PRD §3: allocate the 8 extended field buffers
    // (gate, type, note, dur, chance, time, phase, sample_id) and emit the
    // corresponding SEQPAT_GATE/TYPE/FIELD/PHASE opcodes. Same wiring as
    // emit_per_voice_seqpat so %.note, %.dur, etc. work on transformed patterns.
    if (!gen.emit_extended_field_buffers(*payload, state_id, call_loc)) {
        return TypedValue::void_val();
    }

    // Phase 2.1 PRD §11: emit per-key SEQPAT_PROP buffers for custom properties
    // collected by SequenceCompiler. Covers all transforms going through this
    // helper (slow, fast, rev, transpose, early, late, palindrome, ...).
    if (!gen.emit_custom_property_buffers(compiler, *payload, state_id)) {
        return TypedValue::void_val();
    }

    // Pattern-as-value: the local SequenceCompiler's mappings already reflect
    // any nested .bank()/.variant() applied through compile_pattern_for_transform,
    // so projecting them here gives the correct bank-qualified sample refs
    // regardless of how this transform is wrapped.
    payload->sample_refs = sample_refs_from_mappings(compiler.sample_mappings());
    gen.publish_sample_refs(payload->sample_refs);

    auto tv = TypedValue::make_pattern(payload, result_buf);
    node_types[node] = tv;
    return tv;
}

CodeGenerator::PatternQuerySource CodeGenerator::emit_pattern_query_only(
    std::uint32_t state_id,
    float cycle_length,
    const SequenceCompiler& compiler,
    std::vector<std::vector<cedar::Event>>& sequence_events,
    const SourceLocation& pattern_loc) {

    PatternQuerySource src;
    compiler.collect_samples(required_samples_);

    // Compute max_voices from the local (possibly post-voicing) events.
    std::uint8_t max_voices = 1;
    for (const auto& seq : sequence_events) {
        for (const auto& e : seq) {
            if (e.num_values > max_voices) max_voices = e.num_values;
        }
    }

    // Emit SEQPAT_QUERY. SEQPAT_STEP / extended fields are emitted by the
    // downstream readout against the transform state_id.
    codegen::InstructionBuilder(cedar::Opcode::SEQPAT_QUERY)
        .output(0xFFFF)
        .state_id(state_id)
        .emit(*this);

    // Store sequence program initialization data for the inner SequenceState.
    codegen::StateInitBuilder::sequence_program(state_id)
        .cycle_length(cycle_length)
        .sequences(compiler.sequences())
        .sequence_events(std::move(sequence_events))
        .total_events(compiler.total_events())
        .is_sample_pattern(compiler.is_sample_pattern())
        .pattern_location(pattern_loc)
        .sequence_sample_mappings(compiler.sample_mappings())
        .publish(*this);

    src.ok = true;
    src.state_id = state_id;
    src.cycle_length = cycle_length;
    src.is_sample_pattern = compiler.is_sample_pattern();
    src.max_voices = max_voices;
    src.total_events = compiler.total_events();
    src.sample_refs = sample_refs_from_mappings(compiler.sample_mappings());
    publish_sample_refs(src.sample_refs);
    for (const auto& [key, slot] : compiler.custom_property_slots()) {
        src.custom_props.emplace_back(key, slot);
    }
    return src;
}

// Note: the previous `emit_instruction_helper(instructions, inst)` thunk was
// removed in PRD prd-parser-codegen-correctness.md Phase 3 (F2) — it pushed
// to `instructions` directly without updating `source_locations_`. Pattern
// emission now routes through `CodeGenerator::emit()` (passed into
// `emit_pattern_with_state` via its `gen` parameter), which keeps both
// parallel vectors in sync.

// ============================================================================
// fast / slow — runtime rate scaling via EVENT_RATE_SCALE
// ============================================================================
//
// PRD prd-runtime-event-transforms Phase 3. fast/slow no longer mutate
// cycle_length at compile time; they emit an EVENT_RATE_SCALE opcode whose
// output (a modulated beat-position signal) feeds the new SEQPAT_QUERY's
// external-clock input (sequencing.hpp op_seqpat_query, inputs[0]).
//
// Implementation strategy: each fast/slow call recompiles its inner pattern
// into a NEW SequenceState (`compile_pattern_for_transform` +
// `emit_pattern_with_state`) so that two transforms applied to the same
// pattern stay independent. The OLD compile-time `cycle_length` mutation is
// removed — the runtime ERS opcode is the sole authority on playback rate.
// Composition `fast(slow(p, 2), 3)` works because each call adds its own
// ERS upstream of its own SEQPAT_QUERY; the inner slow's ERS runs first,
// the outer fast's ERS reads from the inner via the upstream-phase input.
// For mixed chains with event_map transforms (e.g.
// `fast(transpose(p, 7), 2)`), `compile_pattern_for_transform`'s recursive
// path applies the inner transform at compile time (legacy fallback), so
// the EVENT_MAP doesn't appear in fast's stream — the events are already
// transposed in the SequenceProgram init.

// Shared implementation of fast/slow. `is_fast=true` → rate = factor;
// `is_fast=false` → rate = 1/factor (constant-folded for numeric factors,
// emitted as a DIV for signal-rate factors).
TypedValue CodeGenerator::emit_rate_scale_call(NodeIndex node, const Node& n,
                                               bool is_fast) {
    const char* fn_name = is_fast ? "fast" : "slow";

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", std::string(fn_name) +
                  "() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }

    // Locate the (raw) factor AST node — needed for the signal-rate path.
    NodeIndex factor_arg_raw = NULL_NODE;
    {
        NodeIndex arg = n.first_child;
        std::size_t idx = 0;
        while (arg != NULL_NODE && idx < 1) {
            arg = ast_->arena[arg].next_sibling;
            idx++;
        }
        if (arg == NULL_NODE) {
            error("E130", std::string(fn_name) +
                      "() requires a factor as second argument", n.location);
            return TypedValue::void_val();
        }
        const Node& arg_node = ast_->arena[arg];
        factor_arg_raw = (arg_node.type == NodeType::Argument)
                            ? arg_node.first_child
                            : arg;
    }

    // Compile-time constant factor → static validation + 1/x folding for slow.
    auto factor_const = get_number_arg(*ast_, n, 1);
    if (factor_const.has_value() && *factor_const <= 0.0f) {
        error("E185", std::string(fn_name) +
                  "() requires a positive factor", n.location);
        return TypedValue::void_val();
    }

    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        // Targeted hint for the common mistake of `euclid(...).fast/.slow`:
        // euclid() is a signal generator, not a pattern, so the rate-scale
        // path can't reach it. Point users at the `dur` parameter.
        const Node& parg_node = ast_->arena[pattern_arg];
        if (parg_node.type == NodeType::Call &&
            ctx_->interner->view(parg_node.as_identifier()) == "euclid") {
            error("E133", std::string(fn_name) +
                      "() is not supported on euclid() — euclid is a signal, "
                      "not a pattern. Use the dur parameter to scale span: "
                      "euclid(hits, steps, rot, dur). For example "
                      "euclid(3, 8, 0, 8) doubles the default span.",
                  n.location);
            return TypedValue::void_val();
        }
        error("E133", std::string(fn_name) +
                  "() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    // Recompile the inner pattern into a fresh sequence so multi-use
    // (`slow(f, 2)` and `fast(f, 2)` on the same `f`) stays independent.
    // compile_pattern_for_transform's recursive path applies inner compile-
    // time transforms (transpose/velocity legacy fallback / inner fast/slow
    // cycle_length); the cycle_length we get back already reflects any
    // inner fast/slow nesting.
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg,
                                       sample_registry_, compiler,
                                       pattern_node, pattern_arena, num_elements,
                                       sequence_events, cycle_length)) {
        error("E130", std::string(fn_name) +
                  "() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    // Resolve the factor → a buffer index. Constants are folded (and slow's
    // 1/x done at compile time); signals get a runtime DIV for slow.
    std::uint16_t factor_buf;
    if (factor_const.has_value()) {
        float rate = is_fast ? *factor_const : (1.0f / *factor_const);
        factor_buf = emit_push_const(rate);
        if (factor_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }
    } else {
        TypedValue factor_tv = visit(factor_arg_raw);
        if (factor_tv.error) return factor_tv;
        if (factor_tv.type != ValueType::Signal &&
            factor_tv.type != ValueType::Number) {
            error("E186", std::string(fn_name) +
                      "() factor must be a number or signal", n.location);
            return TypedValue::void_val();
        }
        if (factor_tv.buffer == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }
        if (is_fast) {
            factor_buf = factor_tv.buffer;
        } else {
            // slow(p, sig) → rate = 1.0 / sig at runtime.
            std::uint16_t one_buf =
                emit_push_const(1.0f);
            if (one_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return TypedValue::void_val();
            }
            factor_buf = codegen::InstructionBuilder(cedar::Opcode::DIV)
                             .inputs({one_buf, factor_tv.buffer})
                             .emit(*this, n.location);
            if (factor_buf == BufferAllocator::BUFFER_UNUSED) {
                return TypedValue::void_val();
            }
        }
    }

    // Allocate the pattern's own state_id and emit its full SEQPAT pipeline.
    std::uint32_t call_count = call_counters_[fn_name]++;
    push_path(std::string(fn_name) + "#" + std::to_string(call_count));
    std::uint32_t state_id = compute_state_id();

    const Node& pattern = (*pattern_arena)[pattern_node];
    auto result_tv = emit_pattern_with_state(
        *this, buffers_, state_inits_, required_samples_,
        node_types_, node, state_id, cycle_length,
        compiler, sequence_events, pattern.location, n.location);
    pop_path();

    if (result_tv.buffer == BufferAllocator::BUFFER_UNUSED && !result_tv.pattern) {
        error("E101", "Buffer pool exhausted", n.location);
        return result_tv;
    }

    // Emit EVENT_RATE_SCALE that targets the just-emitted SequenceState
    // (state_id). ERS mutates upstream->cycle_length each block; SEQPAT_*
    // opcodes pick it up automatically. Emission order doesn't matter for
    // correctness — ERS runs each block before the SEQPAT_QUERY because
    // codegen emits it after the pattern but the program loader sequences
    // by instruction order, and ERS reads/writes the same SequenceState as
    // the SEQPAT_QUERY. For the first block, ERS captures the SequenceState's
    // initial cycle_length BEFORE SEQPAT_QUERY consults it; subsequent
    // blocks see the modulated value.
    push_path("ers");
    std::uint32_t ers_state_id = compute_state_id();
    pop_path();
    pop_path();  // back out of fast#N / slow#N path

    auto& stream = emit_stream();
    auto& locs = loc_stream();
    // Upstream SequenceState state_id is packed across inputs[2..3] (same
    // encoding as EVENT_MAP / EVENT_FILTER via event_transform_upstream_id).
    const cedar::Instruction ers =
        codegen::InstructionBuilder(cedar::Opcode::EVENT_RATE_SCALE)
            .output(0xFFFF)
            .input(1, factor_buf)
            .input(2, static_cast<std::uint16_t>(state_id & 0xFFFF))
            .input(3, static_cast<std::uint16_t>((state_id >> 16) & 0xFFFF))
            .state_id(ers_state_id)
            .get();

    // Insert ERS BEFORE the first SEQPAT_* instruction reading this state_id
    // so the cycle_length mutation lands before any opcode consults it on
    // this block. emit_pattern_with_state appends in topological order, so
    // SEQPAT_QUERY is the earliest consumer. PRD prd-parser-codegen-
    // correctness.md Phase 3 (F2): the non-tail insert must touch both
    // `stream` AND `locs` in lock-step or the parallel invariant breaks.
    std::optional<std::size_t> insert_idx;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        if (stream[i].state_id == state_id &&
            stream[i].opcode == cedar::Opcode::SEQPAT_QUERY) {
            insert_idx = i;
            break;
        }
    }
    if (insert_idx) {
        const auto pos = static_cast<std::ptrdiff_t>(*insert_idx);
        stream.insert(stream.begin() + pos, ers);
        locs.insert(locs.begin() + pos, n.location);
    } else {
        // Defensive: emit_pattern_with_state should always emit SEQPAT_QUERY.
        emit(ers);
    }

    codegen::StateInitBuilder::rate_scale(ers_state_id)
        .pattern_location(n.location)
        .publish(*this);

    return result_tv;
}

TypedValue CodeGenerator::handle_slow_call(NodeIndex node, const Node& n) {
    return emit_rate_scale_call(node, n, /*is_fast=*/false);
}

TypedValue CodeGenerator::handle_fast_call(NodeIndex node, const Node& n) {
    return emit_rate_scale_call(node, n, /*is_fast=*/true);
}

// ============================================================================
// EVENT_REORDER — rev / palindrome / zoom / compress (PRD Phase 4)
// ============================================================================
//
// Shared shape for the 4 structural transforms shipped in Commit A:
//   1. compile_pattern_for_transform → fresh sequence_events + cycle_length
//      (recursive compile-time fold remains the legacy fallback for nested
//      const-only chains like fast(rev(p), 2)).
//   2. Allocate inner state_id under "<fn>#N" path; emit SEQPAT_QUERY +
//      StateInitData::SequenceProgram via emit_pattern_query_only.
//   3. Allocate transform state_id under "<fn>#N/reorder" path.
//   4. Emit EVENT_REORDER opcode with kind/flags packed in rate byte and
//      params in inputs[0..1]; inputs[2..3] carry the upstream (inner) state.
//   5. Push StateInitData::Reorder with downstream cycle_length + capacity.
//   6. Re-target the PatternQuerySource to the transform state and call
//      emit_pattern_readout to emit SEQPAT_STEP / extended fields / SAMPLE_PLAY
//      / SEQPAT_PROP against the transform state.
//
// Composes naturally with upstream runtime EVENT_MAP / EVENT_FILTER /
// EVENT_RATE_SCALE — the inner SequenceState's OutputEvents are read each
// block by EVENT_REORDER and the rewritten events feed downstream readout.

TypedValue CodeGenerator::emit_reorder_call(
    NodeIndex node, const Node& n, const char* fn_name,
    std::uint8_t kind, std::uint8_t flags,
    std::uint16_t param0_buf, std::uint16_t param1_buf,
    float cycle_length_factor, std::uint32_t capacity_factor) {

    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE ||
        !compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                       compiler, pattern_node, pattern_arena, num_elements,
                                       sequence_events, cycle_length)) {
        error("E130", std::string(fn_name) +
                  "() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    std::uint32_t call_count = call_counters_[fn_name]++;
    push_path(std::string(fn_name) + "#" + std::to_string(call_count));
    std::uint32_t inner_state_id = compute_state_id();

    const Node& pattern = (*pattern_arena)[pattern_node];
    PatternQuerySource src = emit_pattern_query_only(
        inner_state_id, cycle_length, compiler, sequence_events, pattern.location);
    if (!src.ok) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    push_path("reorder");
    std::uint32_t transform_state_id = compute_state_id();
    pop_path();

    codegen::InstructionBuilder(cedar::Opcode::EVENT_REORDER)
        .rate(cedar::event_reorder_rate(kind, flags))
        .output(0xFFFF)
        .inputs({param0_buf, param1_buf,
                 static_cast<std::uint16_t>(inner_state_id & 0xFFFF),
                 static_cast<std::uint16_t>((inner_state_id >> 16) & 0xFFFF)})
        .state_id(transform_state_id)
        .emit(*this);

    // Downstream transform state: holds the rewritten OutputEvents that the
    // readout reads via SEQPAT_STEP / SEQPAT_FIELD etc.
    // OutputEvents capacity: max(upstream * fanout_factor, 32). The state
    // pool clamps to a 32-floor and rounds up internally.
    codegen::StateInitBuilder::reorder(transform_state_id)
        .cycle_length(cycle_length * cycle_length_factor)
        .is_sample_pattern(compiler.is_sample_pattern())
        .total_events(compiler.total_events() * capacity_factor)
        .pattern_location(pattern.location)
        .publish(*this);

    pop_path();  // out of "<fn>#N"

    // Re-target the readout to the transform state, with the kind-scaled
    // downstream cycle_length stamped on the PatternPayload.
    src.state_id = transform_state_id;
    src.cycle_length = cycle_length * cycle_length_factor;
    return emit_pattern_readout(node, src, n.location);
}

// ============================================================================
// EVENT_FANOUT — ply / linger / segment (PRD Phase 4 Commit B)
// ============================================================================
//
// Same structure as emit_reorder_call: compile inner pattern, emit inner
// SEQPAT_QUERY + StateInitData::SequenceProgram via emit_pattern_query_only,
// allocate transform state_id, emit EVENT_FANOUT instruction + Fanout init,
// re-target the readout. Differs from emit_reorder_call only in the opcode
// emitted and the StateInitData type tag.

TypedValue CodeGenerator::emit_fanout_call(
    NodeIndex node, const Node& n, const char* fn_name,
    std::uint8_t kind, std::uint16_t param0_buf,
    float cycle_length_factor, std::uint32_t capacity_factor) {

    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE ||
        !compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                       compiler, pattern_node, pattern_arena, num_elements,
                                       sequence_events, cycle_length)) {
        error("E130", std::string(fn_name) +
                  "() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    std::uint32_t call_count = call_counters_[fn_name]++;
    push_path(std::string(fn_name) + "#" + std::to_string(call_count));
    std::uint32_t inner_state_id = compute_state_id();

    const Node& pattern = (*pattern_arena)[pattern_node];
    PatternQuerySource src = emit_pattern_query_only(
        inner_state_id, cycle_length, compiler, sequence_events, pattern.location);
    if (!src.ok) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    push_path("fanout");
    std::uint32_t transform_state_id = compute_state_id();
    pop_path();

    codegen::InstructionBuilder(cedar::Opcode::EVENT_FANOUT)
        .rate(cedar::event_fanout_rate(kind))
        .output(0xFFFF)
        .input(0, param0_buf)
        .input(2, static_cast<std::uint16_t>(inner_state_id & 0xFFFF))
        .input(3, static_cast<std::uint16_t>((inner_state_id >> 16) & 0xFFFF))
        .state_id(transform_state_id)
        .emit(*this);

    codegen::StateInitBuilder::fanout(transform_state_id)
        .cycle_length(cycle_length * cycle_length_factor)
        .is_sample_pattern(compiler.is_sample_pattern())
        .total_events(compiler.total_events() * capacity_factor)
        .pattern_location(pattern.location)
        .publish(*this);

    pop_path();  // out of "<fn>#N"

    src.state_id = transform_state_id;
    src.cycle_length = cycle_length * cycle_length_factor;
    return emit_pattern_readout(node, src, n.location);
}


TypedValue CodeGenerator::emit_pattern_readout(NodeIndex node,
                                               const PatternQuerySource& src,
                                               SourceLocation loc) {
    const std::uint8_t max_voices = (src.max_voices < 1) ? 1 : src.max_voices;

    std::uint16_t value_buf = alloc_buffer(loc);
    std::uint16_t velocity_buf = alloc_buffer(loc);
    std::uint16_t trigger_buf = alloc_buffer(loc);
    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    // Per-voice SEQPAT_STEP reading the final transform's SequenceState.
    std::vector<std::uint16_t> voice_buffers;
    for (std::uint8_t voice = 0; voice < max_voices; ++voice) {
        std::uint16_t voice_value_buf =
            (voice == 0) ? value_buf : alloc_buffer(loc);
        if (voice_value_buf == BufferAllocator::BUFFER_UNUSED) {
            return TypedValue::void_val();
        }
        codegen::InstructionBuilder(cedar::Opcode::SEQPAT_STEP)
            .input(0, (voice == 0) ? velocity_buf : 0xFFFF)
            .input(1, (voice == 0) ? trigger_buf : 0xFFFF)
            .input(2, voice)
            .output(voice_value_buf)
            .state_id(src.state_id)
            .emit(*this);
        voice_buffers.push_back(voice_value_buf);
    }

    auto payload = std::make_shared<PatternPayload>();
    payload->fields[PatternPayload::FREQ] = value_buf;
    payload->fields[PatternPayload::VEL] = velocity_buf;
    payload->fields[PatternPayload::TRIG] = trigger_buf;
    payload->state_id = src.state_id;
    payload->cycle_length = src.cycle_length;
    payload->is_sample_pattern = src.is_sample_pattern;
    payload->max_voices = max_voices;
    if (max_voices > 1 && !src.is_sample_pattern) {
        payload->voice_freqs = std::move(voice_buffers);
    }

    std::uint16_t result_buf = value_buf;
    if (src.is_sample_pattern) {
        SamplePatternEmitCtx ctx;
        ctx.kind = SamplePatternEmitCtx::Kind::Pattern;
        ctx.seq_state_id = src.state_id;
        ctx.value_buf = value_buf;
        ctx.trigger_buf = trigger_buf;
        ctx.velocity_buf = velocity_buf;  // EVENT_MAP already scaled velocity
        ctx.loc = loc;
        std::uint16_t output_buf = emit_sample_chain(
            buffers_, [this](const cedar::Instruction& i){ emit(i); }, ctx);
        if (output_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", loc);
            return TypedValue::void_val();
        }
        result_buf = output_buf;
    }

    // Extended field buffers (gate/type/note/dur/chance/time/phase/sample_id).
    if (!emit_extended_field_buffers(*payload, src.state_id, loc)) {
        error("E101", "Buffer pool exhausted", loc);
        return TypedValue::void_val();
    }

    // SEQPAT_PROP buffers for custom properties carried across the chain.
    // EVENT_MAP copies prop_vals/prop_set_mask through unchanged, so the slot
    // indices match the upstream pattern.
    for (const auto& [key, slot] : src.custom_props) {
        const std::uint16_t buf =
            codegen::InstructionBuilder(cedar::Opcode::SEQPAT_PROP)
                .input(0, 0)  // voice 0 (inputs[1] unset = internal clock)
                .rate(slot)
                .state_id(src.state_id)
                .emit(*this, loc);
        if (buf == BufferAllocator::BUFFER_UNUSED) {
            return TypedValue::void_val();
        }
        payload->custom_fields[key] = buf;
        payload->custom_field_slots[key] = slot;
    }

    payload->sample_refs = src.sample_refs;
    publish_sample_refs(payload->sample_refs);

    return cache_and_return(node, TypedValue::make_pattern(payload, result_buf));
}

// PRD prd-codegen-sprawl-cleanup Phase 5 — canonical emitter for the
// SequenceProgram-recompiling transforms (bank / variant / tune / anchor /
// mode). Shared shape: compile the inner pattern, push "<name>#N" onto the
// semantic-ID path, then lower to the SEQPAT pipeline. Per-transform
// variation (tuning context, voicing mutation, sample-mapping mutation,
// per-voice vs single-voice emission) is injected via SeqpatTransformHooks —
// see codegen.hpp for the hook contract. transport and voicing deliberately
// stay standalone (see their handlers).
TypedValue CodeGenerator::emit_seqpat_transform(NodeIndex node, const Node& n,
                                                const char* transform_name,
                                                NodeIndex pattern_arg,
                                                const SeqpatTransformHooks& hooks) {
    // Compile the pattern (may include inner transforms applied recursively)
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    if (hooks.pre_compile) hooks.pre_compile(compiler);

    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, pattern_arena, num_elements,
                                        sequence_events, cycle_length)) {
        error("E130", std::string(transform_name) +
                  "() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    if (hooks.post_compile) hooks.post_compile(compiler, sequence_events);

    // Set up state ID
    std::uint32_t call_count = call_counters_[transform_name]++;
    push_path(std::string(transform_name) + "#" + std::to_string(call_count));
    std::uint32_t state_id = compute_state_id();

    if (hooks.emission == SeqpatTransformHooks::Emission::PerVoice) {
        // tune / anchor / mode: per-voice emission via the shared helper;
        // sample mappings and sample_refs both come from the compiler.
        const Node& pattern = (*pattern_arena)[pattern_node];
        auto result_tv = emit_pattern_with_state(
            *this, buffers_, state_inits_, required_samples_,
            node_types_, node, state_id, cycle_length,
            compiler, sequence_events, pattern.location, n.location);

        pop_path();

        if (result_tv.buffer == BufferAllocator::BUFFER_UNUSED && !result_tv.pattern) {
            error("E101", "Buffer pool exhausted", n.location);
        }

        return result_tv;
    }

    // Emission::SingleVoiceMutatedMappings (bank / variant): copy the
    // compiler's mappings, mutate them, and move the mutated copy into the
    // SequenceProgram state init.
    auto sample_mappings = compiler.sample_mappings();

    if (hooks.mutate_mappings) hooks.mutate_mappings(sample_mappings);

    // Collect required samples (with updated mapping info)
    compiler.collect_samples(required_samples_);

    bool is_sample_pattern = compiler.is_sample_pattern();

    // Allocate buffers for outputs
    std::uint16_t value_buf = alloc_buffer(n.location);
    std::uint16_t velocity_buf = alloc_buffer(n.location);
    std::uint16_t trigger_buf = alloc_buffer(n.location);

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        return TypedValue::void_val();
    }

    // Emit SEQPAT_QUERY instruction
    codegen::InstructionBuilder(cedar::Opcode::SEQPAT_QUERY)
        .output(0xFFFF)
        .state_id(state_id)
        .emit(*this);

    // Emit SEQPAT_STEP
    codegen::InstructionBuilder(cedar::Opcode::SEQPAT_STEP)
        .inputs({velocity_buf, trigger_buf, /*voice*/ 0})
        .output(value_buf)
        .state_id(state_id)
        .emit(*this);

    // Store sequence program initialization data
    const Node& pattern = (*pattern_arena)[pattern_node];
    codegen::StateInitBuilder::sequence_program(state_id)
        .cycle_length(cycle_length)
        .sequences(compiler.sequences())
        .sequence_events(std::move(sequence_events))
        .total_events(compiler.total_events())
        .is_sample_pattern(is_sample_pattern)
        .pattern_location(pattern.location)
        .sequence_sample_mappings(std::move(sample_mappings))
        .publish(*this);

    // Wire up SAMPLE_PLAY for sample patterns. Without this the returned
    // buffer would be raw sample-IDs (DC), not audio.
    std::uint16_t result_buf = value_buf;
    if (is_sample_pattern) {
        SamplePatternEmitCtx ctx;
        ctx.kind = SamplePatternEmitCtx::Kind::Pattern;
        ctx.seq_state_id = state_id;
        ctx.value_buf = value_buf;
        ctx.trigger_buf = trigger_buf;
        ctx.velocity_buf = velocity_buf;
        ctx.loc = n.location;
        std::uint16_t output_buf = emit_sample_chain(
            buffers_, [this](const cedar::Instruction& i){ emit(i); }, ctx);
        if (output_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::void_val();
        }
        result_buf = output_buf;
    }

    // Build PatternPayload
    auto payload = std::make_shared<PatternPayload>();
    payload->fields[PatternPayload::FREQ] = value_buf;
    payload->fields[PatternPayload::VEL] = velocity_buf;
    payload->fields[PatternPayload::TRIG] = trigger_buf;
    payload->state_id = state_id;
    payload->cycle_length = cycle_length;
    payload->is_sample_pattern = is_sample_pattern;
    payload->max_voices = compiler.max_voices();

    // Records-and-field-access PRD §3: populate extended fields so %.field
    // works on transform-mutated patterns.
    if (!emit_extended_field_buffers(*payload, state_id, n.location)) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    // Phase 2.1 PRD §11: emit per-key SEQPAT_PROP buffers for custom properties.
    if (!emit_custom_property_buffers(compiler, *payload, state_id)) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    // The mutated mappings live in state_inits_.back() (we moved the local
    // sample_mappings into it above). Project them into the Pattern's
    // sample_refs so the mutation (bank / variant) travels with the value.
    payload->sample_refs = sample_refs_from_mappings(
        state_inits_.back().sequence_sample_mappings);
    publish_sample_refs(payload->sample_refs);

    pop_path();
    return cache_and_return(node, TypedValue::make_pattern(payload, result_buf));
}

TypedValue CodeGenerator::handle_bank_call(NodeIndex node, const Node& n) {
    // bank(pattern, bank_name) - set sample bank for all events
    // Sets the bank field on all sample mappings for deferred resolution

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto bank_name = get_string_arg(*ast_, n, 1);

    if (pattern_arg == NULL_NODE) {
        error("E130", "bank() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }

    if (!bank_name.has_value()) {
        error("E131", "bank() requires a string as second argument (e.g., \"TR808\")", n.location);
        return TypedValue::void_val();
    }

    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "bank() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    SeqpatTransformHooks hooks;
    hooks.emission = SeqpatTransformHooks::Emission::SingleVoiceMutatedMappings;
    // Update bank field on all sample mappings
    hooks.mutate_mappings = [&](std::vector<SequenceSampleMapping>& sample_mappings) {
        for (auto& mapping : sample_mappings) {
            mapping.bank = *bank_name;
        }
    };
    return emit_seqpat_transform(node, n, "bank", pattern_arg, hooks);
}

TypedValue CodeGenerator::handle_variant_call(NodeIndex node, const Node& n) {
    // variant(pattern, index) - set sample variant for all events
    // Two cases:
    //   1. variant(pattern, number) - fixed variant for all events
    //   2. variant(pattern, pattern) - variant per-event from another pattern

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    NodeIndex variant_arg = get_pattern_arg(*ast_, n, 1);

    if (pattern_arg == NULL_NODE) {
        error("E130", "variant() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }

    if (variant_arg == NULL_NODE) {
        error("E131", "variant() requires an index number or pattern as second argument", n.location);
        return TypedValue::void_val();
    }

    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "variant() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    // Check variant argument type
    const Node& variant_node = ast_->arena[variant_arg];
    bool is_fixed_variant = (variant_node.type == NodeType::NumberLit);
    int fixed_variant = 0;

    if (is_fixed_variant) {
        fixed_variant = static_cast<int>(variant_node.as_number());
        if (fixed_variant < 0) {
            error("E131", "variant() index must be non-negative", n.location);
            return TypedValue::void_val();
        }
    } else if (!is_pattern_node(*ast_, *symbols_, variant_arg, *ctx_->interner)) {
        error("E131", "variant() second argument must be a number or pattern", n.location);
        return TypedValue::void_val();
    }

    SeqpatTransformHooks hooks;
    hooks.emission = SeqpatTransformHooks::Emission::SingleVoiceMutatedMappings;
    hooks.mutate_mappings = [&](std::vector<SequenceSampleMapping>& sample_mappings) {
        if (is_fixed_variant) {
            // Case 1: Fixed variant - set on all sample mappings
            for (auto& mapping : sample_mappings) {
                mapping.variant = static_cast<std::uint8_t>(fixed_variant);
            }
        } else {
            // Case 2: Per-event variant from pattern
            // Compile the variant pattern to get its events
            SequenceCompiler variant_compiler(ast_->arena, sample_registry_);
            NodeIndex variant_pattern_node = NULL_NODE;
            const AstArena* variant_pattern_arena = nullptr;
            std::uint32_t variant_num_elements = 1;
            std::vector<std::vector<cedar::Event>> variant_events;
            float variant_cycle_length = 1.0f;

            if (compile_pattern_for_transform(*this, *ast_, variant_arg, sample_registry_,
                                              variant_compiler, variant_pattern_node, variant_pattern_arena, variant_num_elements,
                                              variant_events, variant_cycle_length)) {

                // Match events: for each sample event in the main pattern,
                // look up the corresponding variant value from the variant pattern.
                // If the variant pattern has fewer events, cycle through them.
                if (!variant_events.empty() && !variant_events[0].empty()) {
                    std::size_t variant_idx = 0;
                    std::size_t variant_count = variant_events[0].size();

                    for (auto& mapping : sample_mappings) {
                        // Get the variant value from the variant pattern
                        const auto& variant_evt = variant_events[0][variant_idx % variant_count];
                        if (variant_evt.type == cedar::EventType::DATA && variant_evt.num_values > 0) {
                            // Use the first value as the variant index
                            int var_val = static_cast<int>(variant_evt.values[0]);
                            if (var_val >= 0) {
                                mapping.variant = static_cast<std::uint8_t>(var_val);
                            }
                        }
                        variant_idx++;
                    }
                }
            }
        }
    };
    return emit_seqpat_transform(node, n, "variant", pattern_arg, hooks);
}

TypedValue CodeGenerator::handle_transport_call(NodeIndex node, const Node& n) {
    // transport(pattern, trig, step?, reset?) - trigger-driven pattern clock
    // Decouples pattern from global BPM by using trigger edges to advance
    //
    // Deliberately not routed through emit_seqpat_transform: transport emits
    // extra SEQPAT_TRANSPORT + ExtendedParams instructions, uses dual state
    // IDs ("transport#N" + nested "pat"), and threads a clock-override buffer
    // through SEQPAT_QUERY / per-voice readout — hooks would contort.

    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "transport() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }

    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "transport() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    // Second arg: trigger signal (required)
    NodeIndex trig_arg = get_pattern_arg(*ast_, n, 1);
    if (trig_arg == NULL_NODE) {
        error("E131", "transport() requires a trigger signal as second argument", n.location);
        return TypedValue::void_val();
    }

    // Third arg: step size (optional, default 1.0)
    NodeIndex step_arg = get_pattern_arg(*ast_, n, 2);

    // Fourth arg: reset trigger (optional)
    NodeIndex reset_arg = get_pattern_arg(*ast_, n, 3);

    // Compile the pattern (may include inner transforms applied recursively)
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;

    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, pattern_arena, num_elements,
                                        sequence_events, cycle_length)) {
        error("E130", "transport() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    // Set up state IDs
    std::uint32_t transport_count = call_counters_["transport"]++;
    push_path("transport#" + std::to_string(transport_count));
    std::uint32_t transport_state_id = compute_state_id();

    // PRD prd-remove-pat-builtin §6.3: keep "pat" path segment unchanged
    // to preserve hot-swap semantic-ID hashes after the pat builtin removal.
    push_path("pat");
    std::uint32_t seq_state_id = compute_state_id();
    pop_path();
    bool is_sample_pattern = compiler.is_sample_pattern();

    // Visit trigger argument
    std::uint16_t trig_buf = visit(trig_arg).buffer;
    if (trig_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Failed to compile trigger argument", n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // Visit optional step argument
    std::uint16_t step_buf = BufferAllocator::BUFFER_UNUSED;
    if (step_arg != NULL_NODE) {
        step_buf = visit(step_arg).buffer;
    }

    // Visit optional reset argument
    std::uint16_t reset_buf = BufferAllocator::BUFFER_UNUSED;
    if (reset_arg != NULL_NODE) {
        reset_buf = visit(reset_arg).buffer;
    }

    // Emit SEQPAT_TRANSPORT (output = beat_pos). cycle_length used to be
    // bit-cast into the inputs[3]/[4] slot pair; prd-extended-params-migration
    // §4.9 moves it to an ExtendedParams<1> companion state, freeing both
    // signal slots.
    const std::uint16_t beat_pos_buf =
        codegen::InstructionBuilder(cedar::Opcode::SEQPAT_TRANSPORT)
            .inputs({trig_buf, step_buf, reset_buf})
            .state_id(transport_state_id)
            .emit(*this, n.location);
    if (beat_pos_buf == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        return TypedValue::void_val();
    }

    // cycle_length → ExtendedParams<1> slot 0 (constant).
    codegen::StateInitBuilder::extended_params(
            cedar::ext_params_state_id(transport_state_id))
        .ext_count(1)
        .ext_slot(0, cycle_length, 0xFFFFu)
        .publish(*this);

    // Emit SEQPAT_QUERY with clock override
    codegen::InstructionBuilder(cedar::Opcode::SEQPAT_QUERY)
        .output(0xFFFF)
        .input(0, beat_pos_buf)  // Clock override
        .state_id(seq_state_id)
        .emit(*this);

    // Collect required samples
    compiler.collect_samples(required_samples_);

    // Allocate buffers for pattern outputs
    std::uint16_t value_buf = alloc_buffer(n.location);
    std::uint16_t velocity_buf = alloc_buffer(n.location);
    std::uint16_t trigger_buf = alloc_buffer(n.location);

    if (value_buf == BufferAllocator::BUFFER_UNUSED ||
        velocity_buf == BufferAllocator::BUFFER_UNUSED ||
        trigger_buf == BufferAllocator::BUFFER_UNUSED) {
        pop_path();
        return TypedValue::void_val();
    }

    // Emit per-voice SEQPAT_STEP/GATE/TYPE with clock override
    std::uint8_t max_voices = compiler.max_voices();
    auto pattern_payload = emit_per_voice_seqpat(node, seq_state_id, max_voices, value_buf, velocity_buf,
                                                  trigger_buf, is_sample_pattern, n.location, beat_pos_buf);
    if (!pattern_payload) {
        pop_path();
        return TypedValue::void_val();
    }

    // Phase 2.1 PRD §11: emit per-key SEQPAT_PROP buffers (with same clock override).
    if (!emit_custom_property_buffers(compiler, *pattern_payload, seq_state_id, beat_pos_buf)) {
        pop_path();
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    // Store sequence program initialization data
    const Node& pattern = (*pattern_arena)[pattern_node];
    codegen::StateInitBuilder::sequence_program(seq_state_id)
        .cycle_length(cycle_length)
        .sequences(compiler.sequences())
        .sequence_events(std::move(sequence_events))
        .total_events(compiler.total_events())
        .is_sample_pattern(is_sample_pattern)
        .pattern_location(pattern.location)
        .sequence_sample_mappings(compiler.sample_mappings())
        .publish(*this);

    pattern_payload->sample_refs = sample_refs_from_mappings(compiler.sample_mappings());
    publish_sample_refs(pattern_payload->sample_refs);

    // Wire up SAMPLE_PLAY for sample patterns. Without this the returned
    // buffer would be raw sample-IDs (DC), not audio.
    std::uint16_t result_buf = value_buf;
    if (is_sample_pattern) {
        SamplePatternEmitCtx ctx;
        ctx.kind = SamplePatternEmitCtx::Kind::Pattern;
        ctx.seq_state_id = seq_state_id;
        ctx.value_buf = value_buf;
        ctx.trigger_buf = trigger_buf;
        ctx.velocity_buf = velocity_buf;
        ctx.loc = n.location;
        std::uint16_t output_buf = emit_sample_chain(
            buffers_, [this](const cedar::Instruction& i){ emit(i); }, ctx);
        if (output_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::void_val();
        }
        result_buf = output_buf;
    }

    pop_path();
    pattern_payload->state_id = seq_state_id;
    pattern_payload->cycle_length = cycle_length;
    return cache_and_return(node, TypedValue::make_pattern(pattern_payload, result_buf));
}

TypedValue CodeGenerator::handle_tune_call(NodeIndex node, const Node& n) {
    // tune(tuning_name, pattern) - apply microtonal tuning context to a pattern
    // tuning_name is a string like "31edo", "24edo"
    // The tuning context affects how micro_offset is resolved to Hz

    auto tuning_name = get_string_arg(*ast_, n, 0);
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 1);

    if (!tuning_name.has_value()) {
        error("E130", "tune() requires a tuning name string as first argument (e.g., \"31edo\")", n.location);
        return TypedValue::void_val();
    }

    if (pattern_arg == NULL_NODE) {
        error("E131", "tune() requires a pattern as second argument", n.location);
        return TypedValue::void_val();
    }

    auto tuning = parse_tuning(*tuning_name);
    if (!tuning.has_value()) {
        error("E132", "Unknown tuning: '" + *tuning_name + "'. Use format like '31edo', '24edo'", n.location);
        return TypedValue::void_val();
    }

    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "tune() second argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    SeqpatTransformHooks hooks;
    // Compile the pattern with the tuning context set
    hooks.pre_compile = [&](SequenceCompiler& compiler) {
        compiler.set_tuning(*tuning);
    };
    return emit_seqpat_transform(node, n, "tune", pattern_arg, hooks);
}

// ============================================================================
// Phase 2 PRD time/structure transform handlers
// ============================================================================


TypedValue CodeGenerator::handle_palindrome_call(NodeIndex node, const Node& n) {
    // palindrome(pattern) — PRD Phase 4. Each block, EVENT_REORDER(PALINDROME)
    // emits each upstream event twice: forward at (0.5*t, 0.5*dur), reverse at
    // (1 - 0.5*t - 0.5*dur, 0.5*dur). Downstream cycle_length is 2x upstream.
    // OutputEvents capacity is 2x upstream to fit the fanout.
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "palindrome() requires a pattern as argument", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "palindrome() argument must be a pattern", n.location);
        return TypedValue::void_val();
    }
    return emit_reorder_call(node, n, "palindrome",
                             cedar::EVENT_REORDER_PALINDROME, /*flags=*/0,
                             /*param0=*/0xFFFF, /*param1=*/0xFFFF,
                             /*cycle_length_factor=*/2.0f,
                             /*capacity_factor=*/2u);
}

std::uint16_t CodeGenerator::resolve_scalar_or_signal_arg(
    const Node& call, std::size_t idx,
    const char* err_code, const char* err_msg) {

    // Locate the raw AST node at position `idx`.
    NodeIndex arg = call.first_child;
    std::size_t i = 0;
    while (arg != NULL_NODE && i < idx) {
        arg = ast_->arena[arg].next_sibling;
        i++;
    }
    if (arg == NULL_NODE) {
        error(err_code, err_msg, call.location);
        return BufferAllocator::BUFFER_UNUSED;
    }
    const Node& arg_node = ast_->arena[arg];
    NodeIndex value_node = (arg_node.type == NodeType::Argument)
                              ? arg_node.first_child : arg;

    // Constant: fold via PUSH_CONST.
    auto const_val = get_number_arg(*ast_, call, idx);
    if (const_val.has_value()) {
        std::uint16_t buf = emit_push_const(static_cast<float>(*const_val));
        if (buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", call.location);
        }
        return buf;
    }

    // Signal: visit and require a Number/Signal-typed buffer.
    TypedValue tv = visit(value_node);
    if (tv.error) {
        return BufferAllocator::BUFFER_UNUSED;
    }
    if (tv.type != ValueType::Signal && tv.type != ValueType::Number) {
        error(err_code, err_msg, call.location);
        return BufferAllocator::BUFFER_UNUSED;
    }
    if (tv.buffer == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", call.location);
    }
    return tv.buffer;
}

TypedValue CodeGenerator::handle_zoom_call(NodeIndex node, const Node& n) {
    // zoom(pattern, start, end) — PRD Phase 4 runtime form: EVENT_REORDER(ZOOM)
    // reads upstream OutputEvents, drops events outside [s, e) and rescales
    // survivors to [0, 1). Both start and end may be signal-rate (sampled at
    // sample 0 each block); degenerate end <= start at runtime → empty output.
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "zoom() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "zoom() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    // Compile-time validation when both are constants — surfaces E132 early.
    auto start_const = get_number_arg(*ast_, n, 1);
    auto end_const = get_number_arg(*ast_, n, 2);
    if (start_const.has_value() && end_const.has_value() &&
        *end_const <= *start_const) {
        error("E132", "zoom() end must be greater than start", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t start_buf = resolve_scalar_or_signal_arg(
        n, 1, "E131",
        "zoom() requires a number or signal as second argument (start)");
    if (start_buf == BufferAllocator::BUFFER_UNUSED) return TypedValue::void_val();
    std::uint16_t end_buf = resolve_scalar_or_signal_arg(
        n, 2, "E131",
        "zoom() requires a number or signal as third argument (end)");
    if (end_buf == BufferAllocator::BUFFER_UNUSED) return TypedValue::void_val();

    return emit_reorder_call(node, n, "zoom",
                             cedar::EVENT_REORDER_ZOOM, /*flags=*/0,
                             start_buf, end_buf,
                             /*cycle_length_factor=*/1.0f,
                             /*capacity_factor=*/1u);
}

TypedValue CodeGenerator::handle_segment_call(NodeIndex node, const Node& n) {
    // segment(pattern, n) — PRD Phase 4 runtime form: EVENT_FANOUT(SEGMENT)
    // samples N evenly-spaced grid points across the cycle and emits one
    // event per grid point, holding the active upstream event at that time
    // (or the latest preceding event). `n` is a compile-time constant —
    // it determines the OutputEvents capacity.
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto n_arg = get_number_arg(*ast_, n, 1);
    if (pattern_arg == NULL_NODE) {
        error("E130", "segment() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!n_arg.has_value() || *n_arg < 1) {
        error("E131",
              "segment() requires a positive integer (>= 1) as second argument",
              n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "segment() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }
    std::uint32_t n_int = static_cast<std::uint32_t>(*n_arg);
    std::uint16_t n_buf = emit_push_const(static_cast<float>(n_int));
    if (n_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }
    return emit_fanout_call(node, n, "segment",
                            cedar::EVENT_FANOUT_SEGMENT, n_buf,
                            /*cycle_length_factor=*/1.0f,
                            /*capacity_factor=*/n_int);
}

// iter / iterBack — PRD Phase 4 Commit C runtime form. Both lower to
// EVENT_REORDER with kind=ITER and the direction flag packed into the rate
// byte's high nibble. The runtime opcode reads ctx.global_sample_counter to
// derive cycle_index and rotates upstream events by -dir * (cycle%n)/n.
// `n` is a compile-time integer constant in [1, 255] (caps at a const
// buffer; n is the rotation period, not a continuous parameter).

TypedValue CodeGenerator::handle_iter_call(NodeIndex node, const Node& n) {
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto n_arg = get_number_arg(*ast_, n, 1);
    if (pattern_arg == NULL_NODE) {
        error("E130", "iter() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!n_arg.has_value() || *n_arg < 1) {
        error("E131", "iter() requires a positive integer (>= 1) as second argument",
              n.location);
        return TypedValue::void_val();
    }
    int n_int = static_cast<int>(*n_arg);
    if (n_int < 1 || n_int > 255) {
        error("E131", "iter() n must be in [1, 255]", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "iter() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }
    std::uint16_t n_buf = emit_push_const(static_cast<float>(n_int));
    if (n_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }
    return emit_reorder_call(node, n, "iter",
                             cedar::EVENT_REORDER_ITER, /*flags=*/0,
                             n_buf, /*param1=*/0xFFFF,
                             /*cycle_length_factor=*/1.0f,
                             /*capacity_factor=*/1u);
}

TypedValue CodeGenerator::handle_iter_back_call(NodeIndex node, const Node& n) {
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto n_arg = get_number_arg(*ast_, n, 1);
    if (pattern_arg == NULL_NODE) {
        error("E130", "iterBack() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!n_arg.has_value() || *n_arg < 1) {
        error("E131",
              "iterBack() requires a positive integer (>= 1) as second argument",
              n.location);
        return TypedValue::void_val();
    }
    int n_int = static_cast<int>(*n_arg);
    if (n_int < 1 || n_int > 255) {
        error("E131", "iterBack() n must be in [1, 255]", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "iterBack() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }
    std::uint16_t n_buf = emit_push_const(static_cast<float>(n_int));
    if (n_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }
    return emit_reorder_call(node, n, "iterBack",
                             cedar::EVENT_REORDER_ITER_BACK,
                             /*flags=*/cedar::EVENT_REORDER_ITER_DIR_BACK,
                             n_buf, /*param1=*/0xFFFF,
                             /*cycle_length_factor=*/1.0f,
                             /*capacity_factor=*/1u);
}

// ============================================================================
// Phase 2 PRD voicing helpers
// ============================================================================

// Apply accumulated voicing state on the compiler to the chord events in
// sequence_events. Walks chord_contexts in lockstep with events[0], voices
// the chord progression, and rewrites cedar::Event values[] with the
// resolved frequencies. Phase 2 PRD §5.4.
static void apply_voicing(SequenceCompiler& compiler,
                          const voicing::VoicingRegistry& registry,
                          std::vector<std::vector<cedar::Event>>& sequence_events) {
    if (sequence_events.empty()) return;
    const auto& contexts = compiler.chord_contexts_root();
    auto& events = sequence_events[0];
    if (contexts.size() != events.size()) return;  // structure mismatch — bail safely

    int anchor = compiler.voicing_anchor();
    if (anchor < 0) anchor = 60;  // PRD §9.3: default c4

    voicing::Mode mode = compiler.voicing_mode();
    // PRD §9.3: anchor without mode → default below (mode_explicit_ false
    // means user set anchor without setting mode — use the default Below).
    // The set_voicing_mode default (Below) already handles this.

    const voicing::VoicingDict* dict = nullptr;
    if (!compiler.voicing_dict_name().empty()) {
        dict = registry.lookup(compiler.voicing_dict_name());
    }
    if (dict == nullptr) dict = registry.lookup("close");

    // Collect chord specs in sequence order.
    std::vector<voicing::ChordSpec> chords;
    std::vector<std::size_t> chord_indices;
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        if (contexts[i].has_value()) {
            chords.push_back(*contexts[i]);
            chord_indices.push_back(i);
        }
    }
    if (chords.empty()) return;

    auto voiced = voicing::voice_chords(chords, anchor, mode, dict);
    for (std::size_t k = 0; k < chord_indices.size() && k < voiced.size(); ++k) {
        std::size_t ev_idx = chord_indices[k];
        auto& ev = events[ev_idx];
        const auto& notes = voiced[k];
        std::size_t num = std::min(notes.size(), static_cast<std::size_t>(cedar::MAX_VALUES_PER_EVENT));
        ev.num_values = static_cast<std::uint8_t>(num);
        for (std::size_t i = 0; i < num; ++i) {
            float midi_f = static_cast<float>(notes[i]);
            float freq = 440.0f * std::pow(2.0f, (midi_f - 69.0f) / 12.0f);
            ev.values[i] = freq;
            // Per-voice MIDI note for the revoiced chord.
            ev.notes[i] = midi_f;
        }
    }
}

// ============================================================================
// Phase 2 PRD generator handlers (run, binary, binaryN)
// ============================================================================

TypedValue CodeGenerator::handle_run_call(NodeIndex node, const Node& n) {
    auto n_arg = get_number_arg(*ast_, n, 0);
    if (!n_arg.has_value() || *n_arg < 0) {
        error("E131", "run() requires a non-negative integer", n.location);
        return TypedValue::void_val();
    }
    int n_int = static_cast<int>(*n_arg);

    auto events = synth_run_events(n_int);
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    compiler.populate_synthetic(std::move(events));

    std::uint32_t cnt = call_counters_["run"]++;
    push_path("run#" + std::to_string(cnt));
    std::uint32_t state_id = compute_state_id();

    auto sequence_events = compiler.sequence_events();
    float cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)

    auto result_tv = emit_pattern_with_state(
        *this, buffers_, state_inits_, required_samples_,
        node_types_, node, state_id, cycle_length,
        compiler, sequence_events, n.location, n.location);

    pop_path();
    if (result_tv.buffer == BufferAllocator::BUFFER_UNUSED && !result_tv.pattern) {
        error("E101", "Buffer pool exhausted", n.location);
    }
    return result_tv;
}

TypedValue CodeGenerator::handle_binary_call(NodeIndex node, const Node& n) {
    auto n_arg = get_number_arg(*ast_, n, 0);
    if (!n_arg.has_value() || *n_arg < 0) {
        error("E131", "binary() requires a non-negative integer", n.location);
        return TypedValue::void_val();
    }
    std::uint32_t n_val = static_cast<std::uint32_t>(*n_arg);
    int bits = compute_binary_bits(n_val);

    auto events = synth_binary_events(n_val, bits);
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    compiler.populate_synthetic(std::move(events));

    std::uint32_t cnt = call_counters_["binary"]++;
    push_path("binary#" + std::to_string(cnt));
    std::uint32_t state_id = compute_state_id();

    auto sequence_events = compiler.sequence_events();
    float cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)

    auto result_tv = emit_pattern_with_state(
        *this, buffers_, state_inits_, required_samples_,
        node_types_, node, state_id, cycle_length,
        compiler, sequence_events, n.location, n.location);

    pop_path();
    if (result_tv.buffer == BufferAllocator::BUFFER_UNUSED && !result_tv.pattern) {
        error("E101", "Buffer pool exhausted", n.location);
    }
    return result_tv;
}

TypedValue CodeGenerator::handle_binary_n_call(NodeIndex node, const Node& n) {
    auto n_arg = get_number_arg(*ast_, n, 0);
    auto bits_arg = get_number_arg(*ast_, n, 1);
    if (!n_arg.has_value() || !bits_arg.has_value()) {
        error("E131", "binaryN() requires two non-negative integers (n, bits)", n.location);
        return TypedValue::void_val();
    }
    if (*n_arg < 0 || *bits_arg < 0) {
        error("E131", "binaryN() requires non-negative integers", n.location);
        return TypedValue::void_val();
    }
    int bits = static_cast<int>(*bits_arg);
    std::uint32_t n_val = static_cast<std::uint32_t>(*n_arg);
    if (bits < 32) {
        n_val &= (1u << bits) - 1u;  // PRD §9.2: truncate
    }

    auto events = synth_binary_events(n_val, bits);
    SequenceCompiler compiler(ast_->arena, sample_registry_);
    compiler.populate_synthetic(std::move(events));

    std::uint32_t cnt = call_counters_["binaryN"]++;
    push_path("binaryN#" + std::to_string(cnt));
    std::uint32_t state_id = compute_state_id();

    auto sequence_events = compiler.sequence_events();
    float cycle_length = 1.0f;  // cycle_length in beats; default 1 (cycle = beat)

    auto result_tv = emit_pattern_with_state(
        *this, buffers_, state_inits_, required_samples_,
        node_types_, node, state_id, cycle_length,
        compiler, sequence_events, n.location, n.location);

    pop_path();
    if (result_tv.buffer == BufferAllocator::BUFFER_UNUSED && !result_tv.pattern) {
        error("E101", "Buffer pool exhausted", n.location);
    }
    return result_tv;
}

// ============================================================================
// Phase 2 PRD voicing handlers (anchor, mode, voicing, addVoicings)
// ============================================================================

TypedValue CodeGenerator::handle_anchor_call(NodeIndex node, const Node& n) {
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto anchor_str = get_string_arg(*ast_, n, 1);
    if (pattern_arg == NULL_NODE) {
        error("E130", "anchor() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!anchor_str.has_value()) {
        error("E131", "anchor() requires a string note name (e.g., \"c4\") as second argument", n.location);
        return TypedValue::void_val();
    }
    auto midi = voicing::parse_anchor(*anchor_str);
    if (!midi.has_value()) {
        error("E140", "anchor() could not parse note name \"" + *anchor_str + "\"", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "anchor() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    SeqpatTransformHooks hooks;
    hooks.post_compile = [&](SequenceCompiler& compiler,
                             std::vector<std::vector<cedar::Event>>& sequence_events) {
        compiler.set_voicing_anchor(*midi);
        apply_voicing(compiler, *ctx_->voicing_registry, sequence_events);
    };
    return emit_seqpat_transform(node, n, "anchor", pattern_arg, hooks);
}

TypedValue CodeGenerator::handle_mode_call(NodeIndex node, const Node& n) {
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto mode_str = get_string_arg(*ast_, n, 1);
    if (pattern_arg == NULL_NODE) {
        error("E130", "mode() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!mode_str.has_value()) {
        error("E131", "mode() requires a string mode name (\"below\"/\"above\"/\"duck\"/\"root\") as second argument", n.location);
        return TypedValue::void_val();
    }
    auto m = voicing::parse_mode(*mode_str);
    if (!m.has_value()) {
        error("E140", "mode() unknown mode \"" + *mode_str + "\"; expected below/above/duck/root", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "mode() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    SeqpatTransformHooks hooks;
    hooks.post_compile = [&](SequenceCompiler& compiler,
                             std::vector<std::vector<cedar::Event>>& sequence_events) {
        compiler.set_voicing_mode(*m);
        apply_voicing(compiler, *ctx_->voicing_registry, sequence_events);
    };
    return emit_seqpat_transform(node, n, "mode", pattern_arg, hooks);
}

TypedValue CodeGenerator::handle_voicing_call(NodeIndex node, const Node& n) {
    // Deliberately not routed through emit_seqpat_transform: voicing does
    // dictionary-lookup + apply, not the canonical compile/mutate/emit shape
    // (PRD Phase 5 reviewer decision).
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    auto name_str = get_string_arg(*ast_, n, 1);
    if (pattern_arg == NULL_NODE) {
        error("E130", "voicing() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!name_str.has_value()) {
        error("E131", "voicing() requires a string dictionary name as second argument", n.location);
        return TypedValue::void_val();
    }
    if (ctx_->voicing_registry->lookup(*name_str) == nullptr) {
        error("E141", "voicing() dictionary \"" + *name_str + "\" not registered (use addVoicings to register)", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "voicing() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    SequenceCompiler compiler(ast_->arena, sample_registry_);
    NodeIndex pattern_node = NULL_NODE;
    const AstArena* pattern_arena = nullptr;
    std::uint32_t num_elements = 1;
    std::vector<std::vector<cedar::Event>> sequence_events;
    float cycle_length = 1.0f;
    if (!compile_pattern_for_transform(*this, *ast_, pattern_arg, sample_registry_,
                                        compiler, pattern_node, pattern_arena, num_elements,
                                        sequence_events, cycle_length)) {
        error("E130", "voicing() failed to compile pattern argument", n.location);
        return TypedValue::void_val();
    }

    compiler.set_voicing_dict(*name_str);
    apply_voicing(compiler, *ctx_->voicing_registry, sequence_events);

    std::uint32_t cnt = call_counters_["voicing"]++;
    push_path("voicing#" + std::to_string(cnt));
    std::uint32_t state_id = compute_state_id();

    const Node& pattern = (*pattern_arena)[pattern_node];
    auto result_tv = emit_pattern_with_state(
        *this, buffers_, state_inits_, required_samples_,
        node_types_, node, state_id, cycle_length,
        compiler, sequence_events, pattern.location, n.location);

    pop_path();
    if (result_tv.buffer == BufferAllocator::BUFFER_UNUSED && !result_tv.pattern) {
        error("E101", "Buffer pool exhausted", n.location);
    }
    return result_tv;
}

TypedValue CodeGenerator::handle_compress_call(NodeIndex node, const Node& n) {
    // compress(pattern, start, end) — PRD Phase 4 runtime form: EVENT_REORDER
    // (COMPRESS) reads upstream OutputEvents and rescales each event's time
    // (and duration) from [0, 1) into [s, e). Both endpoints may be
    // signal-rate; degenerate end <= start → empty output.
    NodeIndex pattern_arg = get_pattern_arg(*ast_, n, 0);
    if (pattern_arg == NULL_NODE) {
        error("E130", "compress() requires a pattern as first argument", n.location);
        return TypedValue::void_val();
    }
    if (!is_pattern_node(*ast_, *symbols_, pattern_arg, *ctx_->interner)) {
        error("E133", "compress() first argument must be a pattern", n.location);
        return TypedValue::void_val();
    }

    auto start_const = get_number_arg(*ast_, n, 1);
    auto end_const = get_number_arg(*ast_, n, 2);
    if (start_const.has_value() && end_const.has_value() &&
        *end_const <= *start_const) {
        error("E132", "compress() end must be greater than start", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t start_buf = resolve_scalar_or_signal_arg(
        n, 1, "E131",
        "compress() requires a number or signal as second argument (start)");
    if (start_buf == BufferAllocator::BUFFER_UNUSED) return TypedValue::void_val();
    std::uint16_t end_buf = resolve_scalar_or_signal_arg(
        n, 2, "E131",
        "compress() requires a number or signal as third argument (end)");
    if (end_buf == BufferAllocator::BUFFER_UNUSED) return TypedValue::void_val();

    return emit_reorder_call(node, n, "compress",
                             cedar::EVENT_REORDER_COMPRESS, /*flags=*/0,
                             start_buf, end_buf,
                             /*cycle_length_factor=*/1.0f,
                             /*capacity_factor=*/1u);
}

} // namespace akkado
