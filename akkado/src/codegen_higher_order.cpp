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
// Bare use of the parameter (`v` in `osc("sin", v)`) still reads the primary
// freq buffer, so the pre-L3 single-signal form keeps compiling unchanged.

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"

#include <set>
#include <string>

namespace akkado {

namespace {

// Recursively collect field names accessed as `<param>.field` anywhere in the
// AST subtree rooted at `root`. Used to decide whether the event-record bank
// is needed and to reject fields the event model cannot supply.
void collect_param_fields(const AstArena& arena, NodeIndex root,
                          const std::string& param,
                          std::set<std::string>& out) {
    if (root == NULL_NODE) return;
    const Node& nd = arena[root];
    if (nd.type == NodeType::FieldAccess) {
        NodeIndex recv = nd.first_child;
        if (recv != NULL_NODE) {
            const Node& r = arena[recv];
            if (r.type == NodeType::Identifier &&
                std::holds_alternative<Node::IdentifierData>(r.data) &&
                r.as_identifier() == param) {
                out.insert(nd.as_field_access().field_name);
            }
        }
    }
    for (NodeIndex c = nd.first_child; c != NULL_NODE;
         c = arena[c].next_sibling) {
        collect_param_fields(arena, c, param, out);
    }
}

// Map an event-record field alias to a slot. Return value:
//   -1  primary freq slot
//   0..6  record-bank offset (vel/dur/note/chance/time/gate/trig)
//   -2  not a valid event-record field
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
    return -2;
}

// Build the event-record TypedValue. `freq_buf` is the primary slot; when
// `bank_buf` is allocated the seven derived fields are wired to the bank.
TypedValue build_event_record(std::uint16_t freq_buf, std::uint16_t bank_buf) {
    std::unordered_map<std::string, TypedValue> fields;
    auto add = [&](std::initializer_list<const char*> aliases,
                   std::uint16_t buf) {
        for (const char* a : aliases) fields.emplace(a, TypedValue::signal(buf));
    };
    add({"freq", "frequency", "pitch", "f", "p"}, freq_buf);
    if (bank_buf != BufferAllocator::BUFFER_UNUSED) {
        add({"vel", "velocity", "v"},      bank_buf + 0);
        add({"dur", "duration"},           bank_buf + 1);
        add({"note", "midi", "n"},         bank_buf + 2);
        add({"chance"},                    bank_buf + 3);
        add({"time", "t0", "start"},       bank_buf + 4);
        add({"gate", "g"},                 bank_buf + 5);
        add({"trig", "trigger"},           bank_buf + 6);
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
        } else if (in_tv.type == ValueType::EventSource && in_tv.event_source) {
            seq_state_id = in_tv.event_source->state_id;
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
    collect_param_fields(ast_->arena, func_ref->closure_node, record_param,
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
        if (slot >= 0) need_bank = true;
    }

    // Convention-slot buffers. freq is the primary slot; the record bank is a
    // contiguous 7-buffer run, allocated only when a non-freq field is used.
    std::uint16_t freq_buf = buffers_.allocate();
    if (freq_buf == UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }
    std::uint16_t bank_buf = UNUSED;
    if (need_bank) {
        bank_buf = buffers_.allocate();
        bool ok = (bank_buf != UNUSED);
        std::uint16_t prev = bank_buf;
        for (int i = 1; i < 7 && ok; ++i) {
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
        voice_out_l = buffers_.allocate();
        voice_out_r = buffers_.allocate();
        mix_l = buffers_.allocate();
        mix_r = buffers_.allocate();
        if (voice_out_l == UNUSED || voice_out_r == UNUSED ||
            mix_l == UNUSED || mix_r == UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }
        if (voice_out_r != voice_out_l + 1 || mix_r != mix_l + 1) {
            error("E166", "Internal error: each_voice stereo buffers not "
                  "adjacent", n.location);
            return TypedValue::void_val();
        }
    } else if (is_reduce) {  // reduce — accumulator slot + mono result
        acc_buf = buffers_.allocate();
        result_buf = buffers_.allocate();
        if (acc_buf == UNUSED || result_buf == UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }
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
        sym.name_hash = fnv1a_hash(record_param);
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
            // Body is the last child (preceding children are parameters —
            // Identifier or DestructureParam nodes).
            const Node& closure_node = ast_->arena[func_ref->closure_node];
            NodeIndex body = NULL_NODE;
            for (NodeIndex child = closure_node.first_child; child != NULL_NODE;
                 child = ast_->arena[child].next_sibling) {
                body = child;
            }
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
    cedar::Instruction fe{};
    fe.opcode = cedar::Opcode::FOREACH_EVENT;
    fe.rate = 0;
    fe.state_id = state_id;
    for (auto& in : fe.inputs) in = UNUSED;
    std::uint8_t allocator_kind;
    if (is_reduce) {
        allocator_kind = 2;  // SHARED
        fe.out_buffer = result_buf;
        fe.inputs[0] = acc_buf;
        fe.inputs[1] = seed_buf;
        fe.inputs[2] = freq_buf;
        fe.inputs[3] = bank_buf;
    } else {
        allocator_kind = 1;  // PER_ITERATION
        fe.inputs[0] = freq_buf;
        fe.inputs[1] = bank_buf;
        if (kind == 0) {  // each_voice — mixed stereo output
            fe.out_buffer = mix_l;
            fe.inputs[4] = voice_out_l;
            fe.flags = cedar::InstructionFlag::STEREO_OUTPUT;
        } else {          // each — side-effecting sink, no mix
            fe.out_buffer = UNUSED;
        }
    }
    emit(fe);

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

}  // namespace akkado
