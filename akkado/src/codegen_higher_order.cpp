// Higher-order DSL codegen — PRD prd-runtime-functions-control-flow L3 §4.3.3/§7.5.
//
// Three operators, all compiling to a FOREACH_EVENT opcode + a subprogram
// block body:
//   each_voice(input, lambda) — per-event instrument, mixes every iteration's
//                               stereo output (PER_ITERATION allocator).
//   each(input, lambda)       — side-effecting per-event sink; the body calls
//                               out() itself (PER_ITERATION, no mix).
//   reduce(input, fn, init)   — threads an accumulator through every event
//                               (SHARED allocator); returns the accumulator.
//                               Reached via the polymorphic handle_reduce_call
//                               (codegen_arrays.cpp) when its operand is a
//                               pattern rather than an array.
//
// Event-record lambda parameter: the per-event parameter `n` is a Record whose
// fields (`n.freq`, `n.vel`, `n.gate`, …) resolve to convention-slot buffers.
// Bare use of the parameter (`v` in `sine(v)`) still reads the primary
// freq buffer, so the pre-L3 single-signal form keeps compiling unchanged.

#include "akkado/codegen.hpp"
#include "akkado/compile_context.hpp"
#include "akkado/string_interner.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/codegen/instruction_builder.hpp"

#include <cedar/opcodes/event_transform_encoding.hpp>

#include <set>
#include <string>

namespace akkado {

namespace {

// Recursively collect field names accessed as `<param>.field` anywhere in the
// AST subtree rooted at `root`. Used to decide whether the event-record bank
// is needed and to reject fields the event model cannot supply.
void collect_param_fields(const AstArena& arena, NodeIndex root,
                          const std::string& param,
                          const StringInterner& interner,
                          std::set<std::string>& out) {
    if (root == NULL_NODE) return;
    const Node& nd = arena[root];
    if (nd.type == NodeType::FieldAccess) {
        NodeIndex recv = nd.first_child;
        if (recv != NULL_NODE) {
            const Node& r = arena[recv];
            if (r.type == NodeType::Identifier &&
                std::holds_alternative<Node::IdentifierData>(r.data) &&
                interner.view(r.as_identifier()) == param) {
                out.insert(nd.as_field_access().field_name);
            }
        }
    }
    for (NodeIndex c = nd.first_child; c != NULL_NODE;
         c = arena[c].next_sibling) {
        collect_param_fields(arena, c, param, interner, out);
    }
}

// Map an event-record field alias to a slot. Return value:
//   -3  chord-array field (notes/freqs) — needs bank with chord-array slots
//   -2  not a valid event-record field
//   -1  primary freq slot
//   0..6  scalar record-bank offset (vel/dur/note/chance/time/gate/trig)
int field_slot(const std::string& name) {
    if (name == "freq" || name == "frequency" || name == "pitch" ||
        name == "f" || name == "p")                       return -1;
    if (name == "vel" || name == "velocity" || name == "v")  return 0;
    if (name == "dur" || name == "duration")                 return 1;
    if (name == "note" || name == "midi" || name == "n")     return 2;
    if (name == "chance")                                    return 3;
    if (name == "time" || name == "t0" || name == "start")   return 4;
    if (name == "gate" || name == "g")                       return 5;
    if (name == "trig" || name == "trigger")                 return 6;
    // Phase 5 Commit D: chord-array reads. The closure body sees
    // `e.notes` and `e.freqs` as DynArrays — wired in build_event_record.
    if (name == "notes" || name == "freqs")                  return -3;
    return -2;
}

// Map a closure-returned record field name to a writable EVENT_OUT_* slot.
// Returns -1 for a name that is not a writable event field. Phase 5 Commit E
// adds the chord-array slots: `notes` -> NOTES_DATA, `freqs` -> FREQS_DATA;
// both also drive a paired write to NUM_VALUES carrying the array length.
int event_map_out_slot(const std::string& name) {
    if (name == "note" || name == "midi" || name == "n")    return cedar::EVENT_OUT_NOTE;
    if (name == "vel" || name == "velocity" || name == "v") return cedar::EVENT_OUT_VEL;
    if (name == "dur" || name == "duration")                return cedar::EVENT_OUT_DUR;
    if (name == "time")                                     return cedar::EVENT_OUT_TIME;
    if (name == "chance")                                   return cedar::EVENT_OUT_CHANCE;
    if (name == "bend")                                     return cedar::EVENT_OUT_BEND;
    if (name == "at" || name == "aftertouch")               return cedar::EVENT_OUT_AT;
    if (name == "notes")                                    return cedar::EVENT_OUT_NOTES_DATA;
    if (name == "freqs")                                    return cedar::EVENT_OUT_FREQS_DATA;
    return -1;
}

// Build the event-record TypedValue. `freq_buf` is the primary slot; when
// `bank_buf` is allocated the scalar fields (slots 0..6) and the chord-array
// DynArrays (notes/freqs, slots 7..9) are all wired to the bank. The bank
// always carries every slot — the runtime always fills all of EVENT_BANK_COUNT
// per event, so unreferenced fields cost a fill but no codegen.
TypedValue build_event_record(std::uint16_t freq_buf, std::uint16_t bank_buf) {
    std::unordered_map<std::string, TypedValue> fields;
    auto add = [&](std::initializer_list<const char*> aliases,
                   std::uint16_t buf) {
        for (const char* a : aliases) fields.emplace(a, TypedValue::signal(buf));
    };
    add({"freq", "frequency", "pitch", "f", "p"}, freq_buf);
    if (bank_buf != BufferAllocator::BUFFER_UNUSED) {
        add({"vel", "velocity", "v"},      bank_buf + cedar::EVENT_BANK_VEL);
        add({"dur", "duration"},           bank_buf + cedar::EVENT_BANK_DUR);
        add({"note", "midi", "n"},         bank_buf + cedar::EVENT_BANK_NOTE);
        add({"chance"},                    bank_buf + cedar::EVENT_BANK_CHANCE);
        add({"time", "t0", "start"},       bank_buf + cedar::EVENT_BANK_TIME);
        add({"gate", "g"},                 bank_buf + cedar::EVENT_BANK_GATE);
        add({"trig", "trigger"},           bank_buf + cedar::EVENT_BANK_TRIG);
        // Phase 5 Commit D: chord-array fields. Both DynArrays share a single
        // length buffer (num_values is identical for notes/freqs of the same
        // event).
        const std::uint16_t notes_data = bank_buf + cedar::EVENT_BANK_NOTES_DATA;
        const std::uint16_t freqs_data = bank_buf + cedar::EVENT_BANK_FREQS_DATA;
        const std::uint16_t num_values = bank_buf + cedar::EVENT_BANK_NUM_VALUES;
        fields.emplace("notes", TypedValue::make_dyn_array(notes_data, num_values));
        fields.emplace("freqs", TypedValue::make_dyn_array(freqs_data, num_values));
    }
    return TypedValue::make_record(std::move(fields), freq_buf);
}

}  // namespace

// Shared FOREACH_EVENT codegen core. kind: 0=each_voice, 1=each, 2=reduce.
TypedValue CodeGenerator::emit_foreach(NodeIndex node, const Node& n, int kind) {
    using codegen::extract_call_args;
    constexpr std::uint16_t UNUSED = BufferAllocator::BUFFER_UNUSED;

    const bool is_reduce = (kind == 2);
    const bool is_each = (kind == 1);
    const char* name = is_reduce ? "reduce" : (is_each ? "each" : "each_voice");
    const std::size_t argc = is_reduce ? 3 : 2;

    auto args = extract_call_args(ast_->arena, n.first_child, argc, argc);
    if (!args.valid) {
        error("E407", std::string(name) + "() requires " +
              std::to_string(argc) + " arguments", n.location);
        return TypedValue::void_val();
    }
    // Arg order: each_voice/each = (input, lambda); reduce = (input, fn, init)
    // — fn before init, consistent with the array reduce() it unifies with.
    NodeIndex input_arg  = args.nodes[0];
    NodeIndex lambda_arg = args.nodes[1];
    NodeIndex seed_arg   = is_reduce ? args.nodes[2] : NULL_NODE;

    // Operand must resolve to a pattern or a MIDI event source — anything
    // else is E242.
    std::uint32_t seq_state_id = 0;
    {
        auto in_tv = visit(input_arg);
        if (in_tv.pattern) {
            seq_state_id = in_tv.pattern->state_id;
        } else {
            error("E242", "each_voice/each/reduce operand must be a pattern, "
                          "MIDI input, or array literal", n.location);
            return TypedValue::void_val();
        }
        polyphonic_pattern_nodes_.erase(input_arg);
    }

    // reduce's seed expression — wired as an ordinary input signal. Re-read by
    // the VM every block, so a runtime signal seed works.
    std::uint16_t seed_buf = UNUSED;
    if (is_reduce) {
        auto seed_tv = visit(seed_arg);
        seed_buf = seed_tv.buffer;
        if (seed_buf == UNUSED) {
            error("E407", "reduce() seed must be a numeric value or signal",
                  n.location);
            return TypedValue::void_val();
        }
    }

    // Resolve the lambda and check its parameter count.
    auto func_ref = resolve_function_arg(lambda_arg);
    if (!func_ref) {
        error("E403", std::string(name) +
              "() last argument must be a function (lambda)", n.location);
        return TypedValue::void_val();
    }
    const std::size_t want_params = is_reduce ? 2 : 1;
    if (func_ref->params.size() != want_params) {
        if (is_reduce) {
            error("E409", "reduce() lambda must have exactly 2 parameters "
                  "(acc, n), got " +
                  std::to_string(func_ref->params.size()), n.location);
        } else {
            error("E404", std::string(name) + "() lambda must have exactly "
                  "1 parameter, got " +
                  std::to_string(func_ref->params.size()), n.location);
        }
        return TypedValue::void_val();
    }

    // The event-record parameter is the last lambda parameter (reduce's first
    // parameter is the accumulator).
    const std::string& record_param = func_ref->params[want_params - 1].name;

    // Pre-scan the lambda body: which event-record fields does it touch?
    std::set<std::string> fields;
    collect_param_fields(ast_->arena, func_ref->closure_node, record_param, *ctx_->interner,
                         fields);
    bool need_bank = false;
    for (const auto& f : fields) {
        int slot = field_slot(f);
        if (slot == -2) {
            error("E408", "field '" + f + "' is not available on an event "
                  "record — available fields: freq, vel, dur, note, chance, "
                  "time, gate, trig", n.location);
            return TypedValue::void_val();
        }
        // -3 is the chord-array sentinel (notes/freqs) — its data lives in
        // the bank's slots 7..9, so we need the bank too.
        if (slot >= 0 || slot == -3) need_bank = true;
    }

    // Convention-slot buffers. freq is the primary slot; the record bank is a
    // contiguous EVENT_BANK_COUNT-buffer run (10 slots — 7 scalar + 3 chord),
    // allocated only when a non-freq field is used.
    std::uint16_t freq_buf = alloc_buffer(n.location);
    if (freq_buf == UNUSED) {
        return TypedValue::void_val();
    }
    std::uint16_t bank_buf = UNUSED;
    if (need_bank) {
        bank_buf = buffers_.allocate();
        bool ok = (bank_buf != UNUSED);
        std::uint16_t prev = bank_buf;
        for (int i = 1; i < cedar::EVENT_BANK_COUNT && ok; ++i) {
            std::uint16_t b = buffers_.allocate();
            if (b == UNUSED || b != prev + 1) ok = false;
            prev = b;
        }
        if (!ok) {
            error("E166", "Internal error: event-record bank buffers not "
                  "contiguous", n.location);
            return TypedValue::void_val();
        }
    }

    // Mode-specific buffers.
    std::uint16_t voice_out_l = UNUSED, voice_out_r = UNUSED;
    std::uint16_t mix_l = UNUSED, mix_r = UNUSED;
    std::uint16_t acc_buf = UNUSED, result_buf = UNUSED;
    if (kind == 0) {  // each_voice — stereo voice-out + stereo mix bus
        voice_out_l = alloc_buffer(n.location);
        if (voice_out_l == UNUSED) return TypedValue::void_val();
        voice_out_r = alloc_buffer(n.location);
        if (voice_out_r == UNUSED) return TypedValue::void_val();
        mix_l = alloc_buffer(n.location);
        if (mix_l == UNUSED) return TypedValue::void_val();
        mix_r = alloc_buffer(n.location);
        if (mix_r == UNUSED) return TypedValue::void_val();
        if (voice_out_r != voice_out_l + 1 || mix_r != mix_l + 1) {
            error("E166", "Internal error: each_voice stereo buffers not "
                  "adjacent", n.location);
            return TypedValue::void_val();
        }
    } else if (is_reduce) {  // reduce — accumulator slot + mono result
        acc_buf = alloc_buffer(n.location);
        if (acc_buf == UNUSED) return TypedValue::void_val();
        result_buf = alloc_buffer(n.location);
        if (result_buf == UNUSED) return TypedValue::void_val();
    }

    std::uint32_t count = call_counters_[name]++;
    push_path(std::string(name) + "#" + std::to_string(count));
    std::uint32_t state_id = compute_state_id();

    // Compile the lambda body into a subprogram block.
    std::uint32_t block_id = begin_subprogram();

    symbols_->push_scope();
    if (is_reduce) {
        symbols_->define_variable(func_ref->params[0].name, acc_buf);
    }
    // The event-record parameter: a Variable whose bare use reads freq_buf and
    // whose `.field` access resolves through the Record typed_value.
    {
        Symbol sym;
        sym.kind = SymbolKind::Variable;
        sym.name = record_param;
        sym.name_id = ctx_->interner->intern(record_param);
        sym.buffer_index = freq_buf;
        sym.typed_value = build_event_record(freq_buf, bank_buf);
        symbols_->define(sym);
    }
    for (const auto& capture : func_ref->captures) {
        symbols_->define_variable(capture.name, capture.buffer_index);
    }

    auto saved_node_types = std::move(node_types_);
    node_types_.clear();

    std::uint16_t body_result = BufferAllocator::BUFFER_UNUSED;
    std::uint16_t body_result_r = 0xFFFF;
    bool body_is_stereo = false;
    {
        TypedValue body_tv = TypedValue::void_val();
        if (func_ref->is_user_function) {
            body_tv = visit(func_ref->closure_node);
        } else {
            NodeIndex body = codegen::closure_body(ast_->arena, func_ref->closure_node);
            if (body != NULL_NODE) body_tv = visit(body);
        }
        body_result = body_tv.buffer;
        body_result_r = body_tv.right_buffer;
        body_is_stereo = body_tv.is_stereo() || is_stereo_buffer(body_result);
        if (body_is_stereo && body_result_r == 0xFFFF &&
            body_result != BufferAllocator::BUFFER_UNUSED) {
            StereoBuffers sb = get_stereo_buffers_by_buffer(body_result);
            body_result = sb.left;
            body_result_r = sb.right;
        }
    }

    if (kind == 0) {
        // each_voice — wire the body result into the stereo voice-out pair
        // (mono broadcasts to both channels).
        if (body_is_stereo) {
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, voice_out_l, body_result));
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, voice_out_r, body_result_r));
        } else if (body_result != BufferAllocator::BUFFER_UNUSED) {
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, voice_out_l, body_result));
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, voice_out_r, body_result));
        }
    } else if (is_reduce) {
        // reduce — the body's final value is the new accumulator. Must be mono.
        if (body_is_stereo) {
            error("E408", "reduce() lambda body must produce a mono value, "
                  "not a stereo signal", n.location);
        }
        if (body_result != BufferAllocator::BUFFER_UNUSED) {
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, result_buf, body_result));
        }
    }
    // each — no result wiring; the body's own out() calls do the work.

    for (auto& [k, v] : saved_node_types) {
        if (node_types_.find(k) == node_types_.end()) node_types_[k] = v;
    }
    symbols_->pop_scope();

    std::size_t body_length = subprograms_[block_id].body.size();
    if (body_length > 255) {
        error("E405", std::string(name) +
              "() lambda body too large (max 255 instructions)", n.location);
        end_subprogram(block_id, 5, 1);
        pop_path();
        return TypedValue::void_val();
    }
    const std::uint8_t output_count = (kind == 0) ? 2 : 1;
    end_subprogram(block_id, /*frame_slot_count=*/5, output_count);

    // Emit the FOREACH_EVENT opcode into the main stream.
    codegen::InstructionBuilder fe(cedar::Opcode::FOREACH_EVENT);
    fe.state_id(state_id);
    std::uint8_t allocator_kind;
    if (is_reduce) {
        allocator_kind = 2;  // SHARED
        fe.output(result_buf)
          .inputs({acc_buf, seed_buf, freq_buf, bank_buf});
    } else {
        allocator_kind = 1;  // PER_ITERATION
        fe.inputs({freq_buf, bank_buf});
        if (kind == 0) {  // each_voice — mixed stereo output
            fe.output(mix_l)
              .input(4, voice_out_l)
              .flags(cedar::InstructionFlag::STEREO_OUTPUT);
        } else {          // each — side-effecting sink, no mix
            fe.output(UNUSED);
        }
    }
    fe.emit(*this);

    pop_path();

    // FOREACH_EVENT instance config.
    StateInitData fe_init;
    fe_init.state_id = state_id;
    fe_init.type = StateInitData::Type::ForeachAlloc;
    fe_init.foreach_allocator_kind = allocator_kind;
    fe_init.foreach_block_id = block_id;
    fe_init.foreach_event_src_state_id = seq_state_id;
    fe_init.foreach_max_iterations = 64;
    fe_init.foreach_field_slot_count = need_bank ? 8 : 1;
    fe_init.foreach_output_count = output_count;
    state_inits_.push_back(std::move(fe_init));

    if (kind == 0) {
        register_stereo(node, mix_l, mix_r);
        return cache_and_return(node, TypedValue::stereo_signal(mix_l, mix_r));
    }
    if (is_reduce) {
        return cache_and_return(node, TypedValue::signal(result_buf));
    }
    return TypedValue::void_val();  // each — side-effecting sink
}

TypedValue CodeGenerator::handle_each_voice_call(NodeIndex node, const Node& n) {
    return emit_foreach(node, n, 0);
}

TypedValue CodeGenerator::handle_each_call(NodeIndex node, const Node& n) {
    return emit_foreach(node, n, 1);
}

// ============================================================================
// event_map / event_filter — closure-taking event-stream transforms
// (PRD prd-runtime-event-transforms Phase 2a)
// ============================================================================
//
// `event_map(events, (e) -> {note: e.note + 7})` and
// `event_filter(events, (e) -> e.vel > 0.5)` compile the closure body into a
// subprogram block and emit a closure EVENT_MAP / EVENT_FILTER opcode that
// runs the block once per upstream event. event_map overlays the closure's
// returned field record onto each event; event_filter drops events whose
// predicate is falsy. The transform owns a downstream SequenceState; the
// pattern readout is emitted against it so `as e |> …` keeps working.
TypedValue CodeGenerator::emit_event_transform(NodeIndex node, const Node& n,
                                               bool is_filter) {
    using codegen::extract_call_args;
    constexpr std::uint16_t UNUSED = BufferAllocator::BUFFER_UNUSED;
    const char* name = is_filter ? "event_filter" : "event_map";

    auto args = extract_call_args(ast_->arena, n.first_child, 2, 2);
    if (!args.valid) {
        error("E407", std::string(name) + "() requires 2 arguments "
              "(events, closure)", n.location);
        return TypedValue::void_val();
    }
    NodeIndex input_arg  = args.nodes[0];
    NodeIndex lambda_arg = args.nodes[1];

    // --- Resolve the upstream event source ---------------------------------
    std::uint32_t upstream_state_id = 0;
    float cycle_length = 1.0f;
    bool is_sample_pattern = false;
    std::uint8_t max_voices = 1;
    std::vector<RequiredSample> sample_refs;
    std::vector<std::pair<std::string, std::uint8_t>> custom_props;
    {
        TypedValue in_tv = visit(input_arg);
        if (in_tv.type == ValueType::Pattern && in_tv.pattern) {
            const PatternPayload& p = *in_tv.pattern;
            upstream_state_id = p.state_id;
            cycle_length      = p.cycle_length;
            is_sample_pattern = p.is_sample_pattern;
            max_voices        = p.max_voices;
            sample_refs       = p.sample_refs;
            for (const auto& [key, slot] : p.custom_field_slots) {
                custom_props.emplace_back(key, slot);
            }
        } else {
            error("E242", std::string(name) + "() first argument must be a "
                  "pattern or MIDI event source", n.location);
            return TypedValue::void_val();
        }
        // The upstream is consumed as an event stream — clear its E410
        // polyphony tracking so a transformed chord is not double-flagged.
        polyphonic_pattern_nodes_.erase(input_arg);
        consume_polyphonic_pattern(upstream_state_id);
    }
    if (upstream_state_id == 0) {
        error("E242", std::string(name) + "() upstream event source has no "
              "runtime state", n.location);
        return TypedValue::void_val();
    }

    // --- Resolve the closure -----------------------------------------------
    auto func_ref = resolve_function_arg(lambda_arg);
    if (!func_ref) {
        error("E403", std::string(name) + "() second argument must be a "
              "closure", n.location);
        return TypedValue::void_val();
    }
    if (func_ref->params.size() != 1) {
        error("E404", std::string(name) + "() closure must have exactly 1 "
              "parameter, got " + std::to_string(func_ref->params.size()),
              n.location);
        return TypedValue::void_val();
    }
    const std::string& record_param = func_ref->params[0].name;

    // Pre-scan: which event-record fields does the closure body read?
    std::set<std::string> fields;
    collect_param_fields(ast_->arena, func_ref->closure_node, record_param, *ctx_->interner,
                         fields);
    bool need_bank = false;
    for (const auto& f : fields) {
        int slot = field_slot(f);
        if (slot == -2) {
            error("E408", "field '" + f + "' is not available on an event "
                  "record — available fields: freq, vel, dur, note, chance, "
                  "time, gate, trig, notes, freqs", n.location);
            return TypedValue::void_val();
        }
        // -3 is the chord-array sentinel; its DynArray data lives in the
        // bank's slots 7..9, so we need the bank too.
        if (slot >= 0 || slot == -3) need_bank = true;
    }

    // --- Allocate the closure's input convention buffers -------------------
    std::uint16_t freq_buf = alloc_buffer(n.location);
    if (freq_buf == UNUSED) {
        return TypedValue::void_val();
    }
    std::uint16_t bank_buf = UNUSED;
    if (need_bank) {
        bank_buf = buffers_.allocate();
        bool ok = (bank_buf != UNUSED);
        std::uint16_t prev = bank_buf;
        for (int i = 1; i < cedar::EVENT_BANK_COUNT && ok; ++i) {
            std::uint16_t b = buffers_.allocate();
            if (b == UNUSED || b != prev + 1) ok = false;
            prev = b;
        }
        if (!ok) {
            error("E166", "Internal error: event-record bank buffers not "
                  "contiguous", n.location);
            return TypedValue::void_val();
        }
    }

    // --- Allocate the closure's output buffers -----------------------------
    // event_map: a contiguous EVENT_OUT_COUNT-buffer output field bank.
    // event_filter: a single predicate result buffer.
    std::uint16_t out_bank = UNUSED;
    std::uint16_t pred_buf = UNUSED;
    if (is_filter) {
        pred_buf = alloc_buffer(n.location);
        if (pred_buf == UNUSED) {
            return TypedValue::void_val();
        }
    } else {
        out_bank = buffers_.allocate();
        bool ok = (out_bank != UNUSED);
        std::uint16_t prev = out_bank;
        for (int i = 1; i < cedar::EVENT_OUT_COUNT && ok; ++i) {
            std::uint16_t b = buffers_.allocate();
            if (b == UNUSED || b != prev + 1) ok = false;
            prev = b;
        }
        if (!ok) {
            error("E166", "Internal error: event-map output bank buffers not "
                  "contiguous", n.location);
            return TypedValue::void_val();
        }
    }

    std::uint32_t count = call_counters_[name]++;
    push_path(std::string(name) + "#" + std::to_string(count));
    std::uint32_t state_id = compute_state_id();

    // --- Compile the closure body into a subprogram block ------------------
    std::uint32_t block_id = begin_subprogram();

    symbols_->push_scope();
    {
        Symbol sym;
        sym.kind = SymbolKind::Variable;
        sym.name = record_param;
        sym.name_id = ctx_->interner->intern(record_param);
        sym.buffer_index = freq_buf;
        sym.typed_value = build_event_record(freq_buf, bank_buf);
        symbols_->define(sym);
    }
    for (const auto& capture : func_ref->captures) {
        symbols_->define_variable(capture.name, capture.buffer_index);
    }

    auto saved_node_types = std::move(node_types_);
    node_types_.clear();

    TypedValue body_tv = TypedValue::void_val();
    if (func_ref->is_user_function) {
        body_tv = visit(func_ref->closure_node);
    } else {
        NodeIndex body = codegen::closure_body(ast_->arena, func_ref->closure_node);
        if (body != NULL_NODE) body_tv = visit(body);
    }

    // Wire the closure result into the transform's output buffers.
    // Mask widened to 16 bits in Phase 5 Commit E (EVENT_OUT_COUNT = 10 now;
    // slots NOTES_DATA/FREQS_DATA/NUM_VALUES bump the high bit past 7).
    std::uint16_t write_mask = 0;
    if (is_filter) {
        if (body_tv.buffer == UNUSED) {
            error("E174", "event_filter() closure must return a numeric "
                  "predicate", n.location);
        } else {
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY,
                                                pred_buf, body_tv.buffer));
        }
    } else {
        if (body_tv.type != ValueType::Record || !body_tv.record) {
            error("E174", "event_map() closure must return a record literal, "
                  "e.g. (e) -> {note: e.note + 7}", n.location);
        } else {
            // Phase 5 Commit E precedence rule: if the closure returns both
            // `note` (scalar) and `notes`/`freqs` (chord array), the array
            // wins and the scalar `note` is dropped. The scalar would
            // otherwise apply BEFORE the array overlay and silently shift
            // the primary voice — surprising. Detect dual-write up front.
            const bool has_chord_array =
                body_tv.record->fields.count("notes") ||
                body_tv.record->fields.count("freqs");

            for (const auto& [fname, fval] : body_tv.record->fields) {
                int slot = event_map_out_slot(fname);
                if (slot < 0) {
                    error("E408", "event_map() closure returned field '" +
                          fname + "' — writable event fields are note, vel, "
                          "dur, time, chance, bend, at, notes, freqs",
                          n.location);
                    continue;
                }
                const bool is_chord_array =
                    (slot == cedar::EVENT_OUT_NOTES_DATA ||
                     slot == cedar::EVENT_OUT_FREQS_DATA);
                if (is_chord_array) {
                    // Resolve the chord-array data + length buffers. DynArray
                    // (from `e.notes` or `map(e.notes, …)`) passes through;
                    // a multi-buffer Array (from `map([literal], …)`) is
                    // packed via ARRAY_PACK + ARRAY_PUSH into a DynArray-
                    // shaped (data, len) pair before the COPY emit. Same
                    // packing shape as the dynamic-index Array path in
                    // codegen.cpp:509-557.
                    std::uint16_t data_buf = UNUSED;
                    std::uint16_t len_buf  = UNUSED;
                    if (fval.type == ValueType::DynArray && fval.dyn) {
                        data_buf = fval.dyn->data_buffer;
                        len_buf  = fval.dyn->len_buffer;
                    } else if (fval.type == ValueType::Array && fval.array) {
                        const auto buffers = buffers_of(fval);
                        if (buffers.empty()) {
                            error("E174", "event_map() field '" + fname +
                                  "' is an empty array", n.location);
                            continue;
                        }
                        const std::uint8_t arr_len =
                            static_cast<std::uint8_t>(buffers.size());
                        const std::uint8_t pack_count =
                            std::min<std::uint8_t>(arr_len, 5);
                        codegen::InstructionBuilder pack(
                            cedar::Opcode::ARRAY_PACK);
                        pack.rate(pack_count);
                        for (std::uint8_t k = 0; k < pack_count; ++k) {
                            pack.input(k, buffers[k]);
                        }
                        std::uint16_t packed = pack.emit(*this, n.location);
                        if (packed == UNUSED) {
                            continue;
                        }
                        for (std::uint8_t k = 5; k < arr_len; ++k) {
                            const std::uint16_t next =
                                codegen::InstructionBuilder(
                                    cedar::Opcode::ARRAY_PUSH)
                                    .input(0, packed)
                                    .input(1, buffers[k])
                                    .rate(k)
                                    .emit(*this, n.location);
                            if (next == UNUSED) {
                                break;
                            }
                            packed = next;
                        }
                        data_buf = packed;
                        len_buf = emit_push_const(static_cast<float>(arr_len));
                    } else {
                        error("E174", "event_map() field '" + fname +
                              "' must be a chord array (e.g. e.notes or "
                              "map(intervals, (i) -> e.note + i)); got a "
                              "scalar", n.location);
                        continue;
                    }
                    if (data_buf == UNUSED || len_buf == UNUSED) continue;

                    // COPY the array's data buffer into the bank's data slot
                    // and the array's length signal into the shared NUM_VALUES
                    // slot. Both mask bits are set so the runtime knows to
                    // apply the chord-array overlay.
                    emit(cedar::Instruction::make_unary(
                        cedar::Opcode::COPY,
                        static_cast<std::uint16_t>(out_bank + slot),
                        data_buf));
                    emit(cedar::Instruction::make_unary(
                        cedar::Opcode::COPY,
                        static_cast<std::uint16_t>(out_bank +
                            cedar::EVENT_OUT_NUM_VALUES),
                        len_buf));
                    write_mask = static_cast<std::uint16_t>(write_mask |
                        (1u << slot) |
                        (1u << cedar::EVENT_OUT_NUM_VALUES));
                    continue;
                }
                // Scalar slot — skip the `note` COPY when a chord array is
                // also present (the array overlay rewrites all voices,
                // including the primary; an extra coupled shift would
                // double-apply).
                if (slot == cedar::EVENT_OUT_NOTE && has_chord_array) {
                    continue;
                }
                if (fval.buffer == UNUSED) {
                    error("E174", "event_map() field '" + fname +
                          "' has no value", n.location);
                    continue;
                }
                emit(cedar::Instruction::make_unary(
                    cedar::Opcode::COPY,
                    static_cast<std::uint16_t>(out_bank + slot), fval.buffer));
                write_mask = static_cast<std::uint16_t>(write_mask | (1u << slot));
            }
        }
    }

    for (auto& [k, v] : saved_node_types) {
        if (node_types_.find(k) == node_types_.end()) node_types_[k] = v;
    }
    symbols_->pop_scope();

    std::size_t body_length = subprograms_[block_id].body.size();
    if (body_length > 255) {
        error("E405", std::string(name) + "() closure body too large "
              "(max 255 instructions)", n.location);
        end_subprogram(block_id, need_bank ? 8 : 1, 1);
        pop_path();
        return TypedValue::void_val();
    }
    end_subprogram(block_id, /*frame_slot_count=*/need_bank ? 8 : 1,
                   /*output_count=*/1);
    pop_path();

    // --- Emit the closure EVENT_MAP / EVENT_FILTER opcode ------------------
    cedar::Instruction et{};
    et.opcode = is_filter ? cedar::Opcode::EVENT_FILTER : cedar::Opcode::EVENT_MAP;
    et.rate = static_cast<std::uint8_t>(block_id);
    et.flags = cedar::InstructionFlag::EVENT_CLOSURE;
    if (!is_filter) {
        et.flags = static_cast<std::uint16_t>(
            et.flags | (write_mask << cedar::InstructionFlag::EVENT_MASK_SHIFT));
    }
    et.out_buffer = UNUSED;
    et.inputs[0] = freq_buf;
    et.inputs[1] = bank_buf;
    et.inputs[2] = static_cast<std::uint16_t>(upstream_state_id & 0xFFFF);
    et.inputs[3] = static_cast<std::uint16_t>((upstream_state_id >> 16) & 0xFFFF);
    et.inputs[4] = is_filter ? pred_buf : out_bank;
    et.state_id = state_id;
    emit(et);

    // EventTransform StateInitData — allocates the transform's OutputEvents
    // buffer. total_events is not known from an already-compiled upstream
    // payload; size generously (the runtime floors to >=32 regardless).
    StateInitData et_init;
    et_init.state_id = state_id;
    et_init.type = StateInitData::Type::EventTransform;
    et_init.cycle_length = cycle_length;
    et_init.is_sample_pattern = is_sample_pattern;
    et_init.total_events = 256;
    state_inits_.push_back(std::move(et_init));

    // --- Emit the readout for the transform-owned SequenceState ------------
    PatternQuerySource src;
    src.ok = true;
    src.state_id = state_id;
    src.cycle_length = cycle_length;
    src.is_sample_pattern = is_sample_pattern;
    src.max_voices = max_voices;
    src.total_events = 256;
    src.sample_refs = std::move(sample_refs);
    src.custom_props = std::move(custom_props);
    // event_map closures that write bend/at surface them as custom props on
    // the readout (fixed prop slots 0/1, matching overlay_event_field).
    if (!is_filter) {
        auto add_prop = [&](const char* key, std::uint8_t prop_slot,
                            std::uint8_t out_slot) {
            if (!(write_mask & (1u << out_slot))) return;
            for (const auto& kv : src.custom_props) {
                if (kv.first == key) return;
            }
            src.custom_props.emplace_back(key, prop_slot);
        };
        add_prop("bend", 0, cedar::EVENT_OUT_BEND);
        add_prop("aftertouch", 1, cedar::EVENT_OUT_AT);
    }
    return emit_pattern_readout(node, src, n.location);
}

TypedValue CodeGenerator::handle_event_map_call(NodeIndex node, const Node& n) {
    return emit_event_transform(node, n, /*is_filter=*/false);
}

TypedValue CodeGenerator::handle_event_filter_call(NodeIndex node, const Node& n) {
    return emit_event_transform(node, n, /*is_filter=*/true);
}

}  // namespace akkado
