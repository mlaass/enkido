// Bus routing codegen — bus()/mixer() calls, closure inlining, master epilogue.

#include "akkado/codegen.hpp"
#include "akkado/named_args.hpp"
#include "akkado/codegen/codegen.hpp"  // Master include for all codegen helpers
#include "akkado/codegen/instruction_builder.hpp"
#include "akkado/codegen/state_init_builder.hpp"
#include "akkado/builtins.hpp"
#include "akkado/overload.hpp"
#include "akkado/compile_context.hpp"
#include "akkado/source_map.hpp"
#include "akkado/stdlib.hpp"
#include "akkado/chord_parser.hpp"
#include "akkado/const_eval.hpp"
#include "akkado/pattern_eval.hpp"
#ifdef CEDAR_HOST_EXTENSIONS
#include "akkado/host_extensions.hpp"
#endif
#include <cedar/vm/state_pool.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>

namespace akkado {

// Use helpers from akkado::codegen namespace
using codegen::encode_const_value;
using codegen::unwrap_argument;
using codegen::is_audio_rate_producer;
using codegen::is_upgradeable_oscillator;
using codegen::upgrade_for_fm;
using codegen::SamplePatternEmitCtx;
using codegen::emit_sample_chain;

// ===========================================================================
// Bus routing — prd-bus-routing Phase 1
// ===========================================================================
// handle_bus_call serves both out() and bus(). out(...) is a pure alias for
// bus(0, ...); routing both through one handler guarantees the byte-identical
// bytecode the PRD requires. It emits an OUTPUT instruction whose out_buffer
// is a bus *placeholder* (bus_placeholder(N)); emit_bus_epilogue later
// allocates the real per-bus scratch buffers and rewrites the placeholders.
TypedValue CodeGenerator::handle_bus_call(NodeIndex node, const Node& n) {
    const std::string func_name = std::string(ctx_->interner->view(n.as_identifier()));  // "out" or "bus"
    const SourceLocation call_loc = n.location;
    current_source_loc_ = call_loc;

    // Gather argument child nodes.
    std::vector<NodeIndex> args;
    for (NodeIndex c = n.first_child; c != NULL_NODE;
         c = ast_->arena[c].next_sibling) {
        args.push_back(c);
    }

    // Resolve `_` placeholders against this builtin's defaults (PRD §10
    // Addendum). out()/bus() have no numeric defaults today so this is a
    // no-op; it locks in the architectural rule for future edits.
    if (const BuiltinInfo* bi = lookup_builtin(func_name)) {
        codegen::resolve_underscore_defaults(
            const_cast<AstArena&>(ast_->arena), *ctx_->interner, args, *bi);
    }

    // Resolve the bus index. out(...) targets bus 0; bus(N, ...) takes a
    // compile-time non-negative integer literal as its first argument.
    int bus_index = 0;
    std::size_t sig_start = 0;
    if (func_name == "bus") {
        if (args.empty()) {
            error("E260", "bus() requires a bus index and a signal argument",
                  call_loc);
            return cache_and_return(node, TypedValue::void_val());
        }
        const Node& idx_arg = ast_->arena[args[0]];
        NodeIndex idx_val = (idx_arg.type == NodeType::Argument)
                                ? idx_arg.first_child : args[0];
        bool ok = idx_val != NULL_NODE &&
                  ast_->arena[idx_val].type == NodeType::NumberLit;
        double raw = ok ? ast_->arena[idx_val].as_number() : 0.0;
        if (ok && (raw < 0.0 || std::floor(raw) != raw)) ok = false;
        if (!ok) {
            error("E260",
                  "bus() index must be a compile-time non-negative integer "
                  "literal", call_loc);
            return cache_and_return(node, TypedValue::void_val());
        }
        if (static_cast<int>(raw) >= MAX_BUS_INDEX) {
            error("E260",
                  "bus() index must be below " + std::to_string(MAX_BUS_INDEX),
                  call_loc);
            return cache_and_return(node, TypedValue::void_val());
        }
        bus_index = static_cast<int>(raw);
        sig_start = 1;
    }

    // Optional trailing string label (OQ4): bus(N, sig, "kick") / out(sig, "mix").
    // Recorded per bus (last non-empty wins) and dropped from the signal args.
    // Only pop when a signal argument would remain — so out("hi") still reports
    // E160 (string is not a signal) rather than silently becoming a label.
    if (args.size() > sig_start + 1) {
        const Node& last = ast_->arena[args.back()];
        NodeIndex last_val =
            (last.type == NodeType::Argument) ? last.first_child : args.back();
        if (last_val != NULL_NODE &&
            ast_->arena[last_val].type == NodeType::StringLit) {
            const std::string& lbl = ast_->arena[last_val].as_string();
            if (!lbl.empty()) bus_labels_[bus_index] = lbl;
            args.pop_back();
        }
    }

    const std::size_t sig_count = args.size() - sig_start;
    if (sig_count < 1 || sig_count > 2) {
        error("E260", func_name + "() expects 1 or 2 signal arguments",
              call_loc);
        return cache_and_return(node, TypedValue::void_val());
    }

    // Resolve a signal argument to its left/right buffers. Mirrors the
    // mono-broadcast / stereo-split handling of the legacy out() path.
    struct Chan { std::uint16_t left; std::uint16_t right; bool stereo; };
    auto resolve = [&](NodeIndex child) -> Chan {
        const Node& an = ast_->arena[child];
        NodeIndex val = (an.type == NodeType::Argument) ? an.first_child : child;
        TypedValue tv = visit(val);
        // Argument type check — mirrors the generic builtin path (E160).
        // out()/bus() signal slots are declared Signal; String/Array/etc.
        // are rejected. Number and Pattern auto-promote (type_compatible).
        if (val != NULL_NODE && !tv.error && tv.type != ValueType::Void &&
            tv.type != ValueType::DynArray &&
            !type_compatible(tv.type, ParamValueType::Signal)) {
            error("E160",
                  func_name + "() argument expects " +
                  param_value_type_name(ParamValueType::Signal) +
                  ", got " + value_type_name(tv.type),
                  ast_->arena[val].location);
        }
        std::uint16_t buf = tv.buffer;
        bool node_stereo = (val != NULL_NODE) && is_stereo(val);
        bool buf_stereo = (buf != BufferAllocator::BUFFER_UNUSED) &&
                          is_stereo_buffer(buf);
        if (node_stereo || buf_stereo) {
            StereoBuffers sb = node_stereo ? get_stereo_buffers(val)
                                           : get_stereo_buffers_by_buffer(buf);
            return {sb.left, sb.right, true};
        }
        return {buf, buf, false};  // mono: broadcast L = R
    };

    // Normal: write to a bus placeholder (emit_bus_epilogue allocates the
    // real buffer and runs the master chain). bypass_master_: write straight
    // to the device sink, raw — no bus, no epilogue (test-only).
    const std::uint16_t dst =
        bypass_master_ ? std::uint16_t{0xFFFF} : bus_placeholder(bus_index);
    const std::uint16_t out_flags =
        bypass_master_ ? std::uint16_t{0}
                       : std::uint16_t{cedar::InstructionFlag::BUS_WRITE};
    auto emit_output = [&](std::uint16_t l, std::uint16_t r) {
        codegen::InstructionBuilder(cedar::Opcode::OUTPUT)
            .inputs({l, r})
            .output(dst)
            .flags(out_flags)
            .emit(*this);
    };

    if (sig_count == 1) {
        Chan c = resolve(args[sig_start]);
        if (!c.stereo && func_name == "bus") {
            warn("W202",
                 "bus(): mono signal auto-broadcast to both channels (L = R)",
                 call_loc);
        }
        current_source_loc_ = call_loc;
        emit_output(c.left, c.right);
    } else {
        Chan a = resolve(args[sig_start]);
        Chan b = resolve(args[sig_start + 1]);
        current_source_loc_ = call_loc;
        if (a.stereo || b.stereo) {
            warn("W185",
                 func_name + "() with mixed channel shapes — auto-escalating: "
                 "mono args broadcast to both buses, stereo args drive both "
                 "buses; contributions sum.", call_loc);
            emit_output(a.left, a.right);
            emit_output(b.left, b.right);
        } else {
            // Explicit L / R: a single OUTPUT with distinct channels.
            emit_output(a.left, b.left);
        }
    }

    return cache_and_return(node, TypedValue::void_val());
}

// handle_mixer_call serves both mixer(N, closure) and master(closure).
// master(c) is a pure alias for mixer(0, c). It validates the call, records a
// MixerCall, and emits NOTHING at the call site — emit_bus_epilogue inlines
// the closure body into the per-bus epilogue (prd-bus-routing Phase 2 §3.3).
TypedValue CodeGenerator::handle_mixer_call(NodeIndex node, const Node& n) {
    const std::string func_name = std::string(ctx_->interner->view(n.as_identifier()));  // "mixer" or "master"
    const SourceLocation call_loc = n.location;
    current_source_loc_ = call_loc;

    // Gather argument child nodes.
    std::vector<NodeIndex> args;
    for (NodeIndex c = n.first_child; c != NULL_NODE;
         c = ast_->arena[c].next_sibling) {
        args.push_back(c);
    }

    // Resolve `_` placeholders against this builtin's defaults (PRD §10
    // Addendum). master()/mixer() have no numeric defaults today; the call
    // is a no-op but locks in the architectural rule for future edits.
    if (const BuiltinInfo* bi = lookup_builtin(func_name)) {
        codegen::resolve_underscore_defaults(
            const_cast<AstArena&>(ast_->arena), *ctx_->interner, args, *bi);
    }

    // Optional trailing string label (OQ4): mixer(N, closure, "drums") /
    // master(closure, "mix"). Popped before arity checks; bound to the bus
    // index once it is resolved below. Only pop when the required args (index +
    // closure) would still remain, so master("hi") keeps its existing error.
    const std::size_t min_args = (func_name == "mixer") ? 2u : 1u;
    std::string pending_label;
    if (args.size() > min_args) {
        const Node& last = ast_->arena[args.back()];
        NodeIndex last_val =
            (last.type == NodeType::Argument) ? last.first_child : args.back();
        if (last_val != NULL_NODE &&
            ast_->arena[last_val].type == NodeType::StringLit) {
            pending_label = ast_->arena[last_val].as_string();
            args.pop_back();
        }
    }
    // Anything still past the required args is a caller mistake. Without this
    // the semantic analyzer's optional_count lets mixer(0, c, 5) through and
    // codegen silently drops the 5.
    if (args.size() > min_args) {
        error("E260",
              func_name + "() takes " + std::to_string(min_args) +
                  " argument(s) plus an optional trailing string label",
              call_loc);
        return cache_and_return(node, TypedValue::void_val());
    }

    // Resolve the bus index. master(...) targets bus 0; mixer(N, ...) takes a
    // compile-time non-negative integer literal as its first argument
    // (validation mirrors handle_bus_call exactly).
    int bus_index = 0;
    NodeIndex closure_arg = NULL_NODE;
    if (func_name == "mixer") {
        if (args.size() < 2) {
            error("E260",
                  "mixer() requires a bus index and a closure argument",
                  call_loc);
            return cache_and_return(node, TypedValue::void_val());
        }
        const Node& idx_arg = ast_->arena[args[0]];
        NodeIndex idx_val = (idx_arg.type == NodeType::Argument)
                                ? idx_arg.first_child : args[0];
        bool ok = idx_val != NULL_NODE &&
                  ast_->arena[idx_val].type == NodeType::NumberLit;
        double raw = ok ? ast_->arena[idx_val].as_number() : 0.0;
        if (ok && (raw < 0.0 || std::floor(raw) != raw)) ok = false;
        if (!ok) {
            error("E260",
                  "mixer() index must be a compile-time non-negative integer "
                  "literal", call_loc);
            return cache_and_return(node, TypedValue::void_val());
        }
        if (static_cast<int>(raw) >= MAX_BUS_INDEX) {
            error("E260",
                  "mixer() index must be below " + std::to_string(MAX_BUS_INDEX),
                  call_loc);
            return cache_and_return(node, TypedValue::void_val());
        }
        bus_index = static_cast<int>(raw);
        closure_arg = args[1];
    } else {  // master
        if (args.empty()) {
            error("E260", "master() requires a closure argument", call_loc);
            return cache_and_return(node, TypedValue::void_val());
        }
        closure_arg = args[0];
    }

    // Bind the popped label to the now-resolved bus index (last non-empty wins).
    if (!pending_label.empty()) bus_labels_[bus_index] = pending_label;

    // Unwrap an Argument wrapper, then require an inline closure literal.
    if (ast_->arena[closure_arg].type == NodeType::Argument) {
        closure_arg = ast_->arena[closure_arg].first_child;
    }
    if (closure_arg == NULL_NODE ||
        ast_->arena[closure_arg].type != NodeType::Closure) {
        error("E260",
              func_name + "() expects an inline closure argument "
              "(e.g. (s) -> s |> ...)", call_loc);
        return cache_and_return(node, TypedValue::void_val());
    }

    // Resolve params + arity via the shared function-arg resolver.
    auto ref = resolve_function_arg(closure_arg);
    if (!ref || ref->is_user_function) {
        error("E260", func_name + "() closure could not be resolved",
              call_loc);
        return cache_and_return(node, TypedValue::void_val());
    }
    // The closure must take exactly 1 (stereo) or 2 (left, right) plain
    // signal parameters — no destructure, no rest param.
    bool arity_ok = (ref->params.size() == 1 || ref->params.size() == 2);
    for (const auto& p : ref->params) {
        if (p.is_destructure || p.is_rest) arity_ok = false;
    }
    if (!arity_ok) {
        error("E262",
              func_name + "() closure must take exactly 1 (stereo) or 2 "
              "(left, right) plain parameters", call_loc);
        return cache_and_return(node, TypedValue::void_val());
    }

    // Locate the closure body (last child of the Closure node) and reject a
    // sink call inside it (out/bus/mixer/master) — rules out routing cycles.
    NodeIndex body = NULL_NODE;
    for (NodeIndex c = ast_->arena[closure_arg].first_child; c != NULL_NODE;
         c = ast_->arena[c].next_sibling) {
        body = c;
    }
    scan_closure_for_sinks(body);

    MixerCall mc;
    mc.bus_index = bus_index;
    mc.closure_node = closure_arg;
    mc.arity = static_cast<int>(ref->params.size());
    mc.param_l = ref->params[0].name;
    if (ref->params.size() == 2) mc.param_r = ref->params[1].name;
    mc.call_loc = call_loc;
    mixer_calls_.push_back(std::move(mc));

    return cache_and_return(node, TypedValue::void_val());
}

// scan_closure_for_sinks recursively walks a mixer/master closure body and
// emits E261 for every out/bus/mixer/master call found inside it. (The <>
// diamond operator is Phase 3 — no token exists yet to scan for.)
bool CodeGenerator::scan_closure_for_sinks(NodeIndex body) {
    if (body == NULL_NODE) return false;
    const Node& n = ast_->arena[body];
    bool found = false;
    if (n.type == NodeType::Call) {
        const std::string callee = std::string(ctx_->interner->view(n.as_identifier()));
        if (callee == "out" || callee == "bus" || callee == "mixer" ||
            callee == "master") {
            error("E261",
                  callee + "() is not allowed inside a mixer/master closure "
                  "body", n.location);
            found = true;
        }
    }
    for (NodeIndex c = n.first_child; c != NULL_NODE;
         c = ast_->arena[c].next_sibling) {
        if (scan_closure_for_sinks(c)) found = true;
    }
    return found;
}

// inline_mixer_closure inlines one mixer/master closure body into the bus
// epilogue, processing the bus stereo pair (bus_l, bus_r) in place.
void CodeGenerator::inline_mixer_closure(const MixerCall& mc,
                                         std::uint16_t bus_l,
                                         std::uint16_t bus_r) {
    // Body = last child of the Closure node.
    NodeIndex body = NULL_NODE;
    for (NodeIndex c = ast_->arena[mc.closure_node].first_child;
         c != NULL_NODE; c = ast_->arena[c].next_sibling) {
        body = c;
    }
    if (body == NULL_NODE) return;

    // Stable semantic-ID path keyed on the bus index (not a call counter) so
    // stateful opcodes inside the closure rebind across recompiles
    // (prd-bus-routing §9 hot-swap).
    push_path("mixer#" + std::to_string(mc.bus_index));
    symbols_->push_scope();

    if (mc.arity == 1) {
        // Bind the single param to the bus stereo pair: the L buffer is the
        // symbol; (L, R) recorded in stereo_buffer_pairs_ so stereo-native
        // ops in the body see a stereo input.
        symbols_->define_variable(mc.param_l, bus_l);
        stereo_buffer_pairs_[bus_l] = bus_r;
    } else {
        // Two mono params: left and right channels separately.
        symbols_->define_variable(mc.param_l, bus_l);
        symbols_->define_variable(mc.param_r, bus_r);
    }

    auto saved_node_types = std::move(node_types_);
    node_types_.clear();

    TypedValue result = visit(body);

    node_types_ = std::move(saved_node_types);
    symbols_->pop_scope();
    pop_path();

    // Resolve the result's stereo pair (variable/alias TypedValues may not
    // carry the R buffer directly — recover it from the legacy stereo map).
    std::uint16_t rl = result.buffer;
    std::uint16_t rr = result.right_buffer;
    bool stereo = result.is_stereo() || is_stereo_buffer(rl);
    if (stereo && rr == 0xFFFF && rl != BufferAllocator::BUFFER_UNUSED) {
        StereoBuffers sb = get_stereo_buffers_by_buffer(rl);
        rl = sb.left;
        rr = sb.right;
    }
    if (rl == BufferAllocator::BUFFER_UNUSED) {
        // Closure produced no value — leave the bus signal unchanged.
        return;
    }

    // Copy the processed result back into the bus pair in place. op_copy is
    // mono-only, so a stereo result needs two COPYs; a mono result is
    // broadcast L = R with a W204 warning (prd-bus-routing §3.3).
    if (stereo) {
        // The closure parameter is bound directly to bus_l (and bus_r is
        // tracked as its stereo pair), so rl/rr may alias the destination
        // bus buffers. Patterns like `stereo(0, left(sg))` yield
        // rr == bus_l, and `stereo(right(sg), left(sg))` yields a full
        // L/R swap. A naive `bus_l ← rl; bus_r ← rr` clobbers bus_l
        // before bus_r reads it. Reorder or use a temp to break the
        // read-after-write hazard.
        const bool swap = (rl == bus_r && rr == bus_l);
        const bool right_reads_bus_l = (rr == bus_l && rl != bus_r);
        if (swap) {
            std::uint16_t tmp = alloc_buffer(mc.call_loc);
            if (tmp == BufferAllocator::BUFFER_UNUSED) {
                return;
            }
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, tmp,
                                                bus_l));
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, bus_l,
                                                bus_r));
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, bus_r,
                                                tmp));
        } else if (right_reads_bus_l) {
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, bus_r,
                                                rr));
            if (rl != bus_l) {
                emit(cedar::Instruction::make_unary(cedar::Opcode::COPY,
                                                    bus_l, rl));
            }
        } else {
            if (rl != bus_l) {
                emit(cedar::Instruction::make_unary(cedar::Opcode::COPY,
                                                    bus_l, rl));
            }
            if (rr != bus_r) {
                emit(cedar::Instruction::make_unary(cedar::Opcode::COPY,
                                                    bus_r, rr));
            }
        }
    } else {
        warn("W204",
             "mixer/master closure returned a mono value — auto-broadcast "
             "L = R", mc.call_loc);
        if (rl != bus_l) {
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, bus_l,
                                                rl));
        }
        emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, bus_r, rl));
    }
}

// emit_bus_epilogue runs once, after the main DAG is generated. It allocates
// the per-bus scratch buffers, rewrites bus placeholders to real indices,
// appends the per-block epilogue (sum non-zero buses into bus 0, default
// soft-clip @ 0.9, forced NaN/clamp safety stage, device store) and prepends
// the prologue that clears every bus accumulator to silence.
void CodeGenerator::emit_bus_epilogue() {
    // Test-only bypass: out()/bus() already emitted plain device writes.
    if (bypass_master_) return;

    current_source_loc_ = SourceLocation{};

    constexpr std::uint16_t kPlaceLo = 0xFF00;
    constexpr std::uint16_t kPlaceHi = 0xFFFE;  // 0xFFFF stays the device sink
    auto is_placeholder = [](std::uint16_t b) {
        return b >= kPlaceLo && b <= kPlaceHi;
    };

    // 1. Collect every referenced bus index from emitted OUTPUT writers
    //    (main stream + subprogram bodies). Bus 0 always exists.
    std::set<int> indices;
    indices.insert(0);
    std::set<int> writer_indices;   // bus indices with ≥1 out()/bus() writer
    std::size_t writer_count = 0;
    auto scan = [&](const std::vector<cedar::Instruction>& code) {
        for (const auto& inst : code) {
            if (inst.opcode == cedar::Opcode::OUTPUT) {
                ++writer_count;
                if (is_placeholder(inst.out_buffer)) {
                    int idx = inst.out_buffer - kPlaceLo;
                    indices.insert(idx);
                    writer_indices.insert(idx);
                }
            }
        }
    };
    scan(instructions_);
    for (const auto& desc : subprograms_) scan(desc.body);

    // Per-bus FX (Phase 2): a mixer(N,…)/master(…) call references a bus even
    // when no out()/bus() writes to it — allocate that bus too.
    for (const auto& mc : mixer_calls_) indices.insert(mc.bus_index);

    // A program with no out()/bus() writer produces no audio — skip the
    // whole bus epilogue. (prd-bus-routing §5.3 specifies an always-emitted
    // epilogue; emitting it only when a sink exists is functionally
    // identical for every audible program and keeps pure-computation
    // snippets free of a silent master chain.)
    if (writer_count == 0) {
        return;
    }

    // 1b. Per-bus FX: resolve mixer/master overrides — the last call per bus
    //     wins (prd-bus-routing §5.4); earlier calls are dropped with W203.
    //     master(c) was recorded as mixer(0, c), so master and an explicit
    //     mixer(0, …) collide naturally.
    std::map<int, MixerCall> winning_mixers;
    for (const auto& mc : mixer_calls_) {
        auto it = winning_mixers.find(mc.bus_index);
        if (it != winning_mixers.end()) {
            warn("W203",
                 "mixer(" + std::to_string(mc.bus_index) + ")/master "
                 "overridden — the closure at line " +
                 std::to_string(it->second.call_loc.line) +
                 " is dropped; the call at line " +
                 std::to_string(mc.call_loc.line) + " takes effect",
                 it->second.call_loc);
        }
        winning_mixers[mc.bus_index] = mc;
    }
    // W205: a non-zero-bus mixer whose bus has no out()/bus() writers
    // processes silence (the prologue still clears the buffer, so it is safe).
    for (const auto& [idx, mc] : winning_mixers) {
        if (idx != 0 && writer_indices.find(idx) == writer_indices.end()) {
            warn("W205",
                 "mixer(" + std::to_string(idx) + ") targets a bus with no "
                 "writers — the closure processes silence", mc.call_loc);
        }
    }

    // 2. Allocate a stereo scratch pair per bus index (ascending order).
    std::map<int, std::uint16_t> bus_left;
    for (int idx : indices) {
        std::uint16_t l = buffers_.allocate();
        std::uint16_t r = buffers_.allocate();
        if (l == BufferAllocator::BUFFER_UNUSED ||
            r == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted (bus routing)", {});
            return;
        }
        if (r != l + 1) {
            error("E166",
                  "Internal error: bus buffer allocation not adjacent", {});
            return;
        }
        bus_left[idx] = l;
    }

    // Publish the bus → buffer-index map so a host can tap individual buses
    // (prd-bus-routing). right = left + 1 (adjacency asserted above).
    bus_buffers_.clear();
    bus_buffers_.reserve(bus_left.size());
    for (const auto& [idx, l] : bus_left) {
        auto lit = bus_labels_.find(idx);
        bus_buffers_.push_back({static_cast<std::uint32_t>(idx), l,
                                static_cast<std::uint16_t>(l + 1),
                                lit != bus_labels_.end() ? lit->second
                                                         : std::string{}});
    }

    // 3. Rewrite bus placeholders to real left-buffer indices.
    auto fixup = [&](std::vector<cedar::Instruction>& code) {
        for (auto& inst : code) {
            if (inst.opcode == cedar::Opcode::OUTPUT &&
                is_placeholder(inst.out_buffer)) {
                inst.out_buffer = bus_left[inst.out_buffer - kPlaceLo];
            }
        }
    };
    fixup(instructions_);
    for (auto& desc : subprograms_) fixup(desc.body);

    const std::uint16_t bus0_l = bus_left[0];
    const std::uint16_t bus0_r = static_cast<std::uint16_t>(bus0_l + 1);

    // 4. Allocate epilogue constant buffers. The chain processes bus 0 in
    //    place, so only two PUSH_CONST scratch slots are needed: `c1` is
    //    reused (soft-clip threshold, then the clamp lower bound) since the
    //    threshold is dead by the time the clamp runs. allocate() is
    //    monotonic — if the last one succeeds all did.
    const std::uint16_t c1 = buffers_.allocate();
    const std::uint16_t c2 = buffers_.allocate();
    // Shared unity (1.0) fallback for every per-bus BUS_TRIM (OQ5 mixer fader).
    const std::uint16_t unity_trim = buffers_.allocate();
    if (unity_trim == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted (bus epilogue)", {});
        return;
    }

    auto emit_push_const = [&](std::uint16_t dst, float value) {
        codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
            .const_value(value)
            .output(dst)
            .emit(*this);
    };

    // Per-bus mixer trim (OQ5): stereo in-place ×gain from the host-poked
    // EnvMap value "__bus_trim_<N>" (unity fallback). Emitted between a bus's
    // mixer closure and its sum into bus 0, so the fader reaches the real master
    // and the post-fader stem tap. Nondestructive: unpoked ⇒ ×1.0 (bit-exact).
    emit_push_const(unity_trim, 1.0f);
    auto emit_bus_trim = [&](std::uint16_t l, int idx) {
        const std::string name = "__bus_trim_" + std::to_string(idx);
        // rate carries the bus index (a compile-time count field, no audio
        // meaning) so the VM can recover this program's bus→buffer map by
        // scanning for BUS_TRIM — used by the per-bus hot-swap crossfade.
        codegen::InstructionBuilder(cedar::Opcode::BUS_TRIM)
            .rate(static_cast<std::uint8_t>(idx))
            .output(l)                  // stereo in place: l, l+1
            .input(1, unity_trim)       // unity fallback
            .state_id(cedar::fnv1a_hash_runtime(name.data(), name.size()))
            .flags(static_cast<std::uint16_t>(
                cedar::InstructionFlag::STEREO_INPUT |
                cedar::InstructionFlag::STEREO_OUTPUT))
            .emit(*this);
    };

    // 5. Emit the epilogue into the main stream.
    // (a) For each non-zero bus: run its mixer closure (if any) on the bus
    //     signal in place, apply the per-bus trim, then sum the bus into bus 0.
    for (const auto& [idx, l] : bus_left) {
        if (idx == 0) continue;
        auto mit = winning_mixers.find(idx);
        if (mit != winning_mixers.end()) {
            inline_mixer_closure(mit->second, l,
                                 static_cast<std::uint16_t>(l + 1));
        }
        emit_bus_trim(l, idx);
        codegen::InstructionBuilder(cedar::Opcode::OUTPUT)
            .inputs({l, static_cast<std::uint16_t>(l + 1)})
            .output(bus0_l)
            .flags(cedar::InstructionFlag::BUS_WRITE)
            .emit(*this);
    }

    // (b) Bus-0 tone chain: the master / mixer(0) closure if one was given,
    //     otherwise the default polynomial soft-clip @ 0.9. Either runs in
    //     place on bus 0; the forced safety stage below always follows.
    {
        auto mit = winning_mixers.find(0);
        if (mit != winning_mixers.end()) {
            inline_mixer_closure(mit->second, bus0_l, bus0_r);
        } else {
            emit_push_const(c1, 0.9f);         // c1 = soft-clip threshold
            // Unwired slots 2/3 (dry/wet) fall back to defaults 0.0 / 1.0.
            codegen::InstructionBuilder(cedar::Opcode::DISTORT_SOFT)
                .inputs({bus0_l, c1})           // STEREO_INPUT → reads bus0_l+1
                .output(bus0_l)                 // in place: bus0_l, bus0_l+1
                .flags(static_cast<std::uint16_t>(
                    cedar::InstructionFlag::STEREO_INPUT |
                    cedar::InstructionFlag::STEREO_OUTPUT))
                .emit(*this);
        }
    }

    // (b2) Master fader (bus 0 trim): post-master-chain, pre-safety, so the
    //      master fader scales the final mix (OQ5). Unity fallback ⇒ bit-exact.
    emit_bus_trim(bus0_l, 0);

    // (c) Forced safety: hard rail at ±1.0 — "do not damage speakers".
    //     std::clamp() passes NaN through unchanged; the device-store
    //     OUTPUT below sanitizes any remaining NaN/Inf to 0. Together they
    //     guarantee the device never sees |sample| > 1.0 or a non-finite
    //     value, regardless of what the bus-0 chain produced.
    emit_push_const(c1, -1.0f);            // c1 reused: clamp lower bound
    emit_push_const(c2, 1.0f);             // c2: clamp upper bound
    emit(cedar::Instruction::make_ternary(cedar::Opcode::CLAMP, bus0_l,
                                          bus0_l, c1, c2));
    emit(cedar::Instruction::make_ternary(cedar::Opcode::CLAMP, bus0_r,
                                          bus0_r, c1, c2));

    // (d) Device store: a plain OUTPUT (no BUS_WRITE flag) accumulates into
    //     the device sinks and sanitizes NaN/Inf → 0 on the way.
    codegen::InstructionBuilder(cedar::Opcode::OUTPUT)
        .inputs({bus0_l, bus0_r})
        .output(0xFFFF)
        .emit(*this);

    // 6. Prologue: clear every bus accumulator to silence before any writer.
    //    Prepended to the main stream — safe because no opcode encodes an
    //    absolute instruction address (control flow uses relative offsets
    //    and the post-finalization subprogram table).
    std::vector<cedar::Instruction> prologue;
    for (const auto& [idx, l] : bus_left) {
        (void)idx;
        prologue.push_back(cedar::Instruction::make_unary(
            cedar::Opcode::COPY, l, cedar::BUFFER_ZERO));
        prologue.push_back(cedar::Instruction::make_unary(
            cedar::Opcode::COPY, static_cast<std::uint16_t>(l + 1),
            cedar::BUFFER_ZERO));
    }
    instructions_.insert(instructions_.begin(),
                         prologue.begin(), prologue.end());
    source_locations_.insert(source_locations_.begin(),
                             prologue.size(), SourceLocation{});

    // Prepending shifts every main-stream instruction index. Patch the
    // absolute-index metadata recorded during visit(). (Control-flow
    // offsets are relative and subprogram offsets are resolved after this,
    // so only scalar_sample_mappings_ needs fixing.)
    const auto shift = static_cast<std::uint32_t>(prologue.size());
    for (auto& m : scalar_sample_mappings_) {
        m.instruction_index += shift;
    }
}

} // namespace akkado
