// Array higher-order function codegen implementations
// Extracted from codegen.cpp for maintainability

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/codegen/instruction_builder.hpp"
#include "akkado/compile_context.hpp"
#include "akkado/const_eval.hpp"
#include "akkado/music_theory.hpp"
#include "akkado/string_interner.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace akkado {

using codegen::unwrap_argument;
using codegen::extract_call_args;
using codegen::get_input_buffers;
using codegen::closure_body;

// Try to resolve an AST node to a compile-time scalar constant.
// Accepts NumberLit directly, or Identifier referencing a const variable.
static std::optional<double> resolve_const_scalar(
    const AstArena& arena, NodeIndex node, const SymbolTable& symbols) {

    if (node == NULL_NODE) return std::nullopt;
    const Node& n = arena[node];

    if (n.type == NodeType::NumberLit) {
        return n.as_number();
    }

    if (n.type == NodeType::BoolLit) {
        return n.as_bool() ? 1.0 : 0.0;
    }

    if (n.type == NodeType::Identifier) {
        if (std::holds_alternative<Node::IdentifierData>(n.data)) {
            // Phase 5 (F12): SymbolTable.lookup(SymbolId) is the no-rehash
            // fast path; no need to materialize the name as a string here.
            auto sym = symbols.lookup(n.as_identifier());
            if (sym && sym->is_const && sym->const_value.has_value()) {
                if (std::holds_alternative<double>(*sym->const_value)) {
                    return std::get<double>(*sym->const_value);
                }
            }
        }
    }

    return std::nullopt;
}

NodeIndex CodeGenerator::resolve_param_literal(NodeIndex node) const {
    return resolve_param_literal_in(node, param_literals_);
}

NodeIndex CodeGenerator::resolve_param_literal_in(
    NodeIndex node,
    const std::unordered_map<std::uint32_t, NodeIndex>& literals) const {
    if (node == NULL_NODE) return node;
    const Node& n = ast_->arena[node];
    if (n.type != NodeType::Identifier) return node;

    std::string name;
    if (std::holds_alternative<Node::IdentifierData>(n.data)) {
        name = ctx_->interner->view(n.as_identifier());
    } else if (std::holds_alternative<Node::ClosureParamData>(n.data)) {
        name = n.as_closure_param().name;
    }
    if (name.empty()) return node;

    auto it = literals.find(fnv1a_hash(name));
    if (it != literals.end()) {
        return it->second;
    }
    return node;
}

// Apply lambda to single buffer
std::uint16_t CodeGenerator::apply_lambda(NodeIndex lambda_node, std::uint16_t arg_buf) {
    const Node& lambda = ast_->arena[lambda_node];
    if (lambda.type != NodeType::Closure) {
        error("E130", "map() second argument must be a lambda (fn => expr)", lambda.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    auto info = codegen::extract_closure_info(ast_->arena, lambda_node, *ctx_->interner);
    NodeIndex body = info.body;

    if (body == NULL_NODE) {
        error("E131", "Lambda has no body", lambda.location);
        return BufferAllocator::BUFFER_UNUSED;
    }
    if (info.params.empty()) {
        error("E132", "Lambda must have at least one parameter", lambda.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    symbols_->push_scope();
    symbols_->define_variable(info.params[0], arg_buf);

    std::uint16_t result;
    {
        NodeTypesFrame frame(*this);
        result = visit(body).buffer;
    }
    symbols_->pop_scope();

    return result;
}

// Resolve function argument (lambda, variable, or function name)
std::optional<FunctionRef> CodeGenerator::resolve_function_arg(NodeIndex func_node) {
    const Node& n = ast_->arena[func_node];

    if (n.type == NodeType::Closure) {
        FunctionRef ref{};
        ref.closure_node = func_node;
        ref.is_user_function = false;

        // Count total children to know how many are params (all except last = body)
        std::size_t child_count = 0;
        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            child_count++;
            child = ast_->arena[child].next_sibling;
        }

        // All children except the last one are parameters
        std::size_t param_count = child_count > 0 ? child_count - 1 : 0;
        child = n.first_child;
        for (std::size_t i = 0; i < param_count && child != NULL_NODE; ++i) {
            const Node& child_node = ast_->arena[child];
            FunctionParamInfo param;
            if (child_node.type == NodeType::DestructureParam) {
                // Inline closure with a `{x, y}` destructure parameter. Mirror
                // the analyzer's `fn`-path handling (analyzer.cpp ~753): a
                // synthetic placeholder name plus the per-field bindings.
                param.name = "__destr_param_" + std::to_string(i);
                param.is_destructure = true;
                param.destructure_fields = destructure_bindings(child_node);
                ref.params.push_back(std::move(param));
            } else if (child_node.type == NodeType::Identifier) {
                if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                    const auto& cp = child_node.as_closure_param();
                    param.name = cp.name;
                    param.default_value = cp.default_value;
                    param.default_string = cp.default_string;
                    param.is_rest = cp.is_rest;
                    if (child_node.first_child != NULL_NODE &&
                        !cp.default_string.has_value()) {
                        param.default_node = child_node.first_child;
                    }
                } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                    param.name = ctx_->interner->view(child_node.as_identifier());
                }
                ref.params.push_back(std::move(param));
            }
            child = ast_->arena[child].next_sibling;
        }
        return ref;
    }

    if (n.type == NodeType::Identifier) {
        std::string name;
        if (std::holds_alternative<Node::IdentifierData>(n.data)) {
            name = ctx_->interner->view(n.as_identifier());
        } else {
            return std::nullopt;
        }

        auto sym = symbols_->lookup(name);
        if (!sym) return std::nullopt;

        if (sym->kind == SymbolKind::FunctionValue) {
            return sym->function_ref;
        }
        if (sym->kind == SymbolKind::UserFunction) {
            // Phase 4: a FunctionRef wraps a single body. Using an overloaded
            // name as a first-class value falls back to the first overload.
            // (This site is probed speculatively in several places, so the
            // fallback stays silent here; call-site dispatch carries the warning.)
            const UserFunctionInfo& fn = sym->primary_overload();
            FunctionRef ref{};
            ref.is_user_function = true;
            ref.user_function_name = sym->name;
            ref.params = fn.params;
            ref.closure_node = fn.body_node;
            return ref;
        }
    }

    return std::nullopt;
}

// Apply a function ref to N argument buffers.
// Binds arg_bufs[i] to ref.params[i].name; the closure may declare more
// parameters than supplied (defaults/unused), but not fewer.
std::uint16_t CodeGenerator::apply_function_ref(const FunctionRef& ref,
                                                std::span<const std::uint16_t> arg_bufs,
                                                SourceLocation loc) {
    // Arity cap guards against pathological / recursive closure bodies.
    if (arg_bufs.size() > 32) {
        error("E132", "Closure arity exceeds maximum of 32", loc);
        return BufferAllocator::BUFFER_UNUSED;
    }
    if (ref.params.size() < arg_bufs.size()) {
        error("E132", "Closure has fewer parameters than arguments supplied", loc);
        return BufferAllocator::BUFFER_UNUSED;
    }

    symbols_->push_scope();
    for (const auto& capture : ref.captures) {
        symbols_->define_variable(capture.name, capture.buffer_index);
    }
    for (std::size_t i = 0; i < arg_bufs.size(); ++i) {
        symbols_->define_variable(ref.params[i].name, arg_bufs[i]);
    }

    std::uint16_t result = BufferAllocator::BUFFER_UNUSED;
    {
        NodeTypesFrame frame(*this);
        if (ref.is_user_function) {
            if (ref.closure_node != NULL_NODE) result = visit(ref.closure_node).buffer;
        } else {
            NodeIndex body = closure_body(ast_->arena, ref.closure_node);
            if (body != NULL_NODE) result = visit(body).buffer;
        }
    }
    symbols_->pop_scope();

    return result;
}

// map(array, fn) — fn is (val) -> ... or (val, idx) -> ...
TypedValue CodeGenerator::handle_map_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E133", "map() requires 2 arguments: map(array, fn)", n.location);
        return TypedValue::void_val();
    }

    auto func_ref = resolve_function_arg(args.nodes[1]);
    if (!func_ref) {
        error("E130", "map() second argument must be a function", n.location);
        return TypedValue::void_val();
    }

    // Dispatch on closure arity: (val) keeps the legacy behavior; (val, idx)
    // additionally receives a per-element index const buffer.
    const std::size_t arity = func_ref->params.size();
    if (arity == 0) {
        error("E132", "map closure must take at least one parameter (the element)",
              n.location);
        return TypedValue::void_val();
    }
    if (arity > 2) {
        error("E146", "map closure takes 1 or 2 parameters: (val) or (val, idx)",
              n.location);
        return TypedValue::void_val();
    }
    const bool with_index = (arity == 2);

    TypedValue array_tv = visit(args.nodes[0]);
    std::uint16_t array_buf = array_tv.buffer;

    // map over a DynArray (PRD prd-pattern-event-arrays §10 Q3): the data is
    // packed in samples 0..len-1 of a single buffer, so a pointwise closure
    // applied to the whole buffer transforms every slot in one shot — no
    // runtime loop opcode. The closure must be pointwise/stateless; per-sample
    // state would be inconsistent across slots. Output is a same-length
    // DynArray sharing the input's runtime len_buffer.
    if (array_tv.type == ValueType::DynArray && array_tv.dyn) {
        if (with_index) {
            error("E183", "map() over a dynamic array supports only a "
                  "1-parameter closure (val); the per-element index form is "
                  "not available for dynamic arrays", n.location);
            return TypedValue::error_val();
        }
        push_path("map#" + std::to_string(call_counters_["map"]++));
        push_path("dyn");
        std::array<std::uint16_t, 1> arg_bufs{array_tv.dyn->data_buffer};
        std::uint16_t result = apply_function_ref(*func_ref, arg_bufs, n.location);
        pop_path();
        pop_path();
        return cache_and_return(
            node, TypedValue::make_dyn_array(result, array_tv.dyn->len_buffer));
    }

    if (!is_multi_buffer(args.nodes[0])) {
        push_path("map#" + std::to_string(call_counters_["map"]++));
        push_path("elem0");
        std::uint16_t result;
        if (with_index) {
            // A non-multi-buffer signal is a 1-element sequence: its index is 0.
            std::uint16_t idx_buf = emit_push_const(0.0f);
            if (idx_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return TypedValue::void_val();
            }
            std::array<std::uint16_t, 2> arg_bufs{array_buf, idx_buf};
            result = apply_function_ref(*func_ref, arg_bufs, n.location);
        } else {
            std::array<std::uint16_t, 1> arg_bufs{array_buf};
            result = apply_function_ref(*func_ref, arg_bufs, n.location);
        }
        pop_path();
        pop_path();
        return cache_and_return(node, TypedValue::signal(result));
    }

    auto element_buffers = get_multi_buffers(args.nodes[0]);
    std::vector<std::uint16_t> result_buffers;

    push_path("map#" + std::to_string(call_counters_["map"]++));
    for (std::size_t i = 0; i < element_buffers.size(); ++i) {
        push_path("elem" + std::to_string(i));
        if (with_index) {
            std::uint16_t idx_buf =
                emit_push_const(static_cast<float>(i));
            if (idx_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return TypedValue::void_val();
            }
            std::array<std::uint16_t, 2> arg_bufs{element_buffers[i], idx_buf};
            result_buffers.push_back(apply_function_ref(*func_ref, arg_bufs, n.location));
        } else {
            std::array<std::uint16_t, 1> arg_bufs{element_buffers[i]};
            result_buffers.push_back(apply_function_ref(*func_ref, arg_bufs, n.location));
        }
        pop_path();
    }
    pop_path();

    return finalize_array_result(node, std::move(result_buffers));
}

// sum(array) or sum(a, b, ...) — variadic, stereo-preserving.
// Sums signals per-channel; if any operand is stereo, the result is stereo
// (mono operands broadcast into both channels). A single array argument is
// summed over its elements (backward-compatible with the old sum(array)).
TypedValue CodeGenerator::handle_sum_call(NodeIndex node, const Node& n) {
    // Collect all argument nodes (variadic): sum is special-cased in the
    // analyzer to allow any arity >= 1, so extract them directly.
    std::vector<NodeIndex> arg_nodes;
    for (NodeIndex arg = n.first_child; arg != NULL_NODE;
         arg = ast_->arena[arg].next_sibling) {
        arg_nodes.push_back(unwrap_argument(ast_->arena, arg));
    }
    if (arg_nodes.empty()) {
        error("E134", "sum() requires at least 1 argument: sum(array) or sum(a, b, ...)",
              n.location);
        return TypedValue::void_val();
    }

    // Flatten every argument into a list of per-channel operands. A mono
    // operand has l == r; a stereo operand carries distinct L/R buffers.
    struct Operand { std::uint16_t l; std::uint16_t r; };
    std::vector<Operand> operands;
    bool any_stereo = false;

    for (NodeIndex arg_node : arg_nodes) {
        TypedValue tv = visit(arg_node);

        if (tv.type == ValueType::Array && tv.array) {
            // Sum over the array's elements.
            for (const TypedValue& elem : tv.array->elements) {
                std::uint16_t eb = elem.buffer;
                bool elem_stereo = elem.is_stereo() || is_stereo_buffer(eb);
                if (elem_stereo) {
                    std::uint16_t er = elem.is_stereo() && elem.right_buffer != 0xFFFF
                        ? elem.right_buffer
                        : get_stereo_buffers_by_buffer(eb).right;
                    operands.push_back({eb, er});
                    any_stereo = true;
                } else {
                    operands.push_back({eb, eb});
                }
            }
            continue;
        }

        // Single signal argument (mono or stereo).
        bool stereo = tv.is_stereo() || is_stereo(arg_node) || is_stereo_buffer(tv.buffer);
        std::uint16_t l = tv.buffer;
        std::uint16_t r = tv.right_buffer;
        if (stereo && r == 0xFFFF) {
            StereoBuffers sb = is_stereo(arg_node)
                ? get_stereo_buffers(arg_node)
                : get_stereo_buffers_by_buffer(l);
            l = sb.left;
            r = sb.right;
        }
        if (stereo) {
            operands.push_back({l, r});
            any_stereo = true;
        } else {
            operands.push_back({l, l});
        }
    }

    // Empty operand list (e.g. sum([])) → zero, matching legacy behavior.
    if (operands.empty()) {
        std::uint16_t zero = emit_zero();
        return cache_and_return(node, TypedValue::signal(zero));
    }
    // Single operand → passthrough (stereo preserved).
    if (operands.size() == 1) {
        const Operand& o = operands[0];
        if (any_stereo) {
            register_stereo(node, o.l, o.r);
            return cache_and_return(node, TypedValue::stereo_signal(o.l, o.r));
        }
        return cache_and_return(node, TypedValue::signal(o.l));
    }

    // In-place accumulator pattern. `op_add` reads each sample before
    // writing the output, so emitting `ADD acc, acc, x` is correctness-
    // preserving and avoids burning N-1 temp buffers for an N-operand sum
    // (the dominant source of buffer-pool exhaustion in wide unison/poly
    // patches).
    auto emit_add = [&](std::uint16_t out, std::uint16_t in0, std::uint16_t in1) {
        // Slot 4 stays 0 (historical zero-init encoding, kept for
        // byte-identical bytecode).
        codegen::InstructionBuilder(cedar::Opcode::ADD)
            .inputs({in0, in1, 0xFFFF, 0xFFFF, 0})
            .output(out)
            .emit(*this);
    };

    // Mono path — single accumulator.
    if (!any_stereo) {
        std::uint16_t acc = alloc_buffer(n.location);
        if (acc == BufferAllocator::BUFFER_UNUSED) {
            return TypedValue::void_val();
        }
        emit_add(acc, operands[0].l, operands[1].l);
        for (std::size_t i = 2; i < operands.size(); ++i) {
            emit_add(acc, acc, operands[i].l);
        }
        return cache_and_return(node, TypedValue::signal(acc));
    }

    // Stereo path — sum L and R independently into an adjacent output pair.
    std::uint16_t out_left = buffers_.allocate();
    std::uint16_t out_right = buffers_.allocate();
    if (out_left == BufferAllocator::BUFFER_UNUSED ||
        out_right == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }
    if (out_right != out_left + 1) {
        error("E166", "Internal error: stereo buffer allocation not adjacent", n.location);
        return TypedValue::void_val();
    }

    emit_add(out_left,  operands[0].l, operands[1].l);
    emit_add(out_right, operands[0].r, operands[1].r);
    for (std::size_t i = 2; i < operands.size(); ++i) {
        emit_add(out_left,  out_left,  operands[i].l);
        emit_add(out_right, out_right, operands[i].r);
    }

    register_stereo(node, out_left, out_right);
    return cache_and_return(node, TypedValue::stereo_signal(out_left, out_right));
}

// reduce(array, fn, init)
TypedValue CodeGenerator::handle_reduce_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 3);
    if (!args.valid) {
        error("E142", "reduce() requires 3 arguments: reduce(coll, fn, init)", n.location);
        return TypedValue::void_val();
    }

    // Polymorphic on operand (PRD prd-runtime-functions-control-flow L3 §7.5):
    // a pattern / MIDI event stream reduces at runtime via
    // FOREACH_EVENT(SHARED); an array reduces by compile-time unrolling below.
    {
        TypedValue operand = visit(args.nodes[0]);
        if (operand.pattern) {
            return emit_foreach(node, n, 2);
        }
    }

    auto func_ref = resolve_function_arg(args.nodes[1]);
    if (!func_ref) {
        error("E143", "reduce() second argument must be a binary function", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t array_buf = visit(args.nodes[0]).buffer;
    std::uint16_t init_buf = visit(args.nodes[2]).buffer;

    auto elem_bufs = is_multi_buffer(args.nodes[0]) ?
        get_multi_buffers(args.nodes[0]) : std::vector<std::uint16_t>{array_buf};

    if (elem_bufs.empty()) {
        return cache_and_return(node, TypedValue::signal(init_buf));
    }

    push_path("reduce#" + std::to_string(call_counters_["reduce"]++));
    std::uint16_t result = init_buf;
    for (std::size_t i = 0; i < elem_bufs.size(); ++i) {
        push_path("step" + std::to_string(i));
        std::array<std::uint16_t, 2> arg_bufs{result, elem_bufs[i]};
        result = apply_function_ref(*func_ref, arg_bufs, n.location);
        pop_path();
    }
    pop_path();

    return cache_and_return(node, TypedValue::signal(result));
}

// zipWith(a, b, fn)
TypedValue CodeGenerator::handle_zipWith_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 3);
    if (!args.valid) {
        error("E144", "zipWith() requires 3 arguments: zipWith(a, b, fn)", n.location);
        return TypedValue::void_val();
    }

    auto func_ref = resolve_function_arg(args.nodes[2]);
    if (!func_ref) {
        error("E145", "zipWith() third argument must be a binary function", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t buf_a = visit(args.nodes[0]).buffer;
    std::uint16_t buf_b = visit(args.nodes[1]).buffer;

    auto buffers_a = is_multi_buffer(args.nodes[0]) ?
        get_multi_buffers(args.nodes[0]) : std::vector<std::uint16_t>{buf_a};
    auto buffers_b = is_multi_buffer(args.nodes[1]) ?
        get_multi_buffers(args.nodes[1]) : std::vector<std::uint16_t>{buf_b};

    std::size_t len = std::min(buffers_a.size(), buffers_b.size());
    if (len == 0) {
        std::uint16_t zero = emit_zero();
        return cache_and_return(node, TypedValue::signal(zero));
    }

    push_path("zipWith#" + std::to_string(call_counters_["zipWith"]++));
    std::vector<std::uint16_t> result_buffers;
    for (std::size_t i = 0; i < len; ++i) {
        push_path("elem" + std::to_string(i));
        std::array<std::uint16_t, 2> arg_bufs{buffers_a[i], buffers_b[i]};
        result_buffers.push_back(apply_function_ref(*func_ref, arg_bufs, n.location));
        pop_path();
    }
    pop_path();

    return finalize_array_result(node, std::move(result_buffers));
}

// zip(a, b) - interleave
TypedValue CodeGenerator::handle_zip_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E146", "zip() requires 2 arguments: zip(a, b)", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t buf_a = visit(args.nodes[0]).buffer;
    std::uint16_t buf_b = visit(args.nodes[1]).buffer;

    auto buffers_a = is_multi_buffer(args.nodes[0]) ?
        get_multi_buffers(args.nodes[0]) : std::vector<std::uint16_t>{buf_a};
    auto buffers_b = is_multi_buffer(args.nodes[1]) ?
        get_multi_buffers(args.nodes[1]) : std::vector<std::uint16_t>{buf_b};

    std::size_t len = std::min(buffers_a.size(), buffers_b.size());
    std::vector<std::uint16_t> result_buffers;
    result_buffers.reserve(len * 2);

    for (std::size_t i = 0; i < len; ++i) {
        result_buffers.push_back(buffers_a[i]);
        result_buffers.push_back(buffers_b[i]);
    }

    return finalize_array_result(node, std::move(result_buffers));
}

// take(n, array)
TypedValue CodeGenerator::handle_take_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E147", "take() requires 2 arguments: take(n, array)", n.location);
        return TypedValue::void_val();
    }

    const Node& n_val = ast_->arena[args.nodes[0]];
    if (n_val.type != NodeType::NumberLit) {
        error("E148", "take() first argument must be a number literal", n_val.location);
        return TypedValue::void_val();
    }

    std::size_t count = static_cast<std::size_t>(n_val.as_number());
    std::uint16_t array_buf = visit(args.nodes[1]).buffer;

    auto elem_bufs = is_multi_buffer(args.nodes[1]) ?
        get_multi_buffers(args.nodes[1]) : std::vector<std::uint16_t>{array_buf};

    count = std::min(count, elem_bufs.size());
    std::vector<std::uint16_t> result_buffers(elem_bufs.begin(), elem_bufs.begin() + count);

    return finalize_array_result(node, std::move(result_buffers));
}

// drop(n, array)
TypedValue CodeGenerator::handle_drop_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E149", "drop() requires 2 arguments: drop(n, array)", n.location);
        return TypedValue::void_val();
    }

    const Node& n_val = ast_->arena[args.nodes[0]];
    if (n_val.type != NodeType::NumberLit) {
        error("E150", "drop() first argument must be a number literal", n_val.location);
        return TypedValue::void_val();
    }

    std::size_t count = static_cast<std::size_t>(n_val.as_number());
    std::uint16_t array_buf = visit(args.nodes[1]).buffer;

    auto elem_bufs = is_multi_buffer(args.nodes[1]) ?
        get_multi_buffers(args.nodes[1]) : std::vector<std::uint16_t>{array_buf};

    count = std::min(count, elem_bufs.size());
    std::vector<std::uint16_t> result_buffers(elem_bufs.begin() + count, elem_bufs.end());

    return finalize_array_result(node, std::move(result_buffers));
}

// reverse(array)
TypedValue CodeGenerator::handle_reverse_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1);
    if (!args.valid) {
        error("E151", "reverse() requires 1 argument: reverse(array)", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t array_buf = visit(args.nodes[0]).buffer;

    if (!is_multi_buffer(args.nodes[0])) {
        return cache_and_return(node, TypedValue::signal(array_buf));
    }

    auto elem_bufs = get_multi_buffers(args.nodes[0]);
    std::reverse(elem_bufs.begin(), elem_bufs.end());

    std::vector<TypedValue> elements;
    for (auto b : elem_bufs) elements.push_back(TypedValue::signal(b));
    return cache_and_return(node, TypedValue::make_array(std::move(elements), elem_bufs[0]));
}

// range(start, end, step=1)
// step is the interval size (always treated as positive); direction is
// determined by start vs end (start > end auto-reverses).
TypedValue CodeGenerator::handle_range_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2, 3);
    if (!args.valid) {
        error("E152", "range() requires 2 or 3 arguments: range(start, end, step=1)", n.location);
        return TypedValue::void_val();
    }

    auto start_val = resolve_const_scalar(ast_->arena, args.nodes[0], *symbols_);
    auto end_val = resolve_const_scalar(ast_->arena, args.nodes[1], *symbols_);
    std::optional<double> step_val = 1.0;
    if (args.nodes.size() == 3) {
        step_val = resolve_const_scalar(ast_->arena, args.nodes[2], *symbols_);
    }

    if (!start_val || !end_val || !step_val) {
        error("E153", "range() arguments must be compile-time constants", n.location);
        return TypedValue::void_val();
    }

    int start = static_cast<int>(*start_val);
    int end = static_cast<int>(*end_val);
    int step_mag = std::abs(static_cast<int>(*step_val));

    if (step_mag == 0) {
        error("E153", "range() step must be non-zero", n.location);
        return TypedValue::void_val();
    }

    std::vector<std::uint16_t> result_buffers;
    int step = (start <= end) ? step_mag : -step_mag;
    auto in_range = [&](int i) { return step > 0 ? i < end : i > end; };

    for (int i = start; in_range(i); i += step) {
        const std::uint16_t buf =
            codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
                .const_value(static_cast<float>(i))
                .emit(*this, n.location);
        if (buf == BufferAllocator::BUFFER_UNUSED) {
            return TypedValue::void_val();
        }

        result_buffers.push_back(buf);
    }

    return finalize_array_result(node, std::move(result_buffers));
}

// repeat(value, n)
TypedValue CodeGenerator::handle_repeat_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E154", "repeat() requires 2 arguments: repeat(value, n)", n.location);
        return TypedValue::void_val();
    }

    const Node& n_val = ast_->arena[args.nodes[1]];
    if (n_val.type != NodeType::NumberLit) {
        error("E155", "repeat() second argument must be a number literal", n_val.location);
        return TypedValue::void_val();
    }

    std::size_t count = static_cast<std::size_t>(n_val.as_number());
    std::uint16_t value_buf = visit(args.nodes[0]).buffer;

    if (count == 0) {
        std::uint16_t zero = emit_zero();
        return cache_and_return(node, TypedValue::signal(zero));
    }
    if (count == 1) {
        return cache_and_return(node, TypedValue::signal(value_buf));
    }

    std::vector<TypedValue> elements;
    elements.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        elements.push_back(TypedValue::signal(value_buf));
    }
    return cache_and_return(node, TypedValue::make_array(std::move(elements), value_buf));
}

// spread(n, source) - force specific voice count (pad with zeros or truncate)
TypedValue CodeGenerator::handle_spread_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E178", "spread() requires 2 arguments: spread(n, source)", n.location);
        return TypedValue::void_val();
    }

    // First argument (n) must be a compile-time constant
    const Node& n_val = ast_->arena[args.nodes[0]];
    if (n_val.type != NodeType::NumberLit) {
        error("E179", "spread() first argument must be a number literal", n_val.location);
        return TypedValue::void_val();
    }

    int target_count = static_cast<int>(n_val.as_number());
    if (target_count < 1 || target_count > 128) {
        error("E180", "spread() count must be between 1 and 128", n_val.location);
        return TypedValue::void_val();
    }

    // Visit the source to get its buffers
    std::uint16_t source_buf = visit(args.nodes[1]).buffer;

    // Get source buffers (multi-buffer or single)
    std::vector<std::uint16_t> source_buffers;
    if (is_multi_buffer(args.nodes[1])) {
        source_buffers = get_multi_buffers(args.nodes[1]);
    } else {
        source_buffers.push_back(source_buf);
    }

    std::size_t target = static_cast<std::size_t>(target_count);
    std::vector<std::uint16_t> result_buffers;
    result_buffers.reserve(target);

    if (source_buffers.size() >= target) {
        // Truncate: take first target_count elements
        for (std::size_t i = 0; i < target; ++i) {
            result_buffers.push_back(source_buffers[i]);
        }
    } else {
        // Pad: copy all source buffers, then pad with zeros
        for (auto buf : source_buffers) {
            result_buffers.push_back(buf);
        }
        // Pad remaining slots with zero buffers
        for (std::size_t i = source_buffers.size(); i < target; ++i) {
            std::uint16_t zero_buf = emit_zero();
            if (zero_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                return TypedValue::void_val();
            }
            result_buffers.push_back(zero_buf);
        }
    }

    // If single result, just return it
    if (result_buffers.size() == 1) {
        return cache_and_return(node, TypedValue::signal(result_buffers[0]));
    }

    // Register as multi-buffer result
    std::vector<TypedValue> elements;
    for (auto b : result_buffers) elements.push_back(TypedValue::signal(b));
    return cache_and_return(node, TypedValue::make_array(std::move(elements), result_buffers[0]));
}

// Handles len(arr) calls - returns compile-time array length
TypedValue CodeGenerator::handle_len_call(NodeIndex node, const Node& n) {
    // Get the argument
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) {
        error("E120", "len() requires exactly 1 argument", n.location);
        return TypedValue::void_val();
    }

    // Unwrap Argument node if present
    const Node& arg_node = ast_->arena[arg];
    NodeIndex arr_node = arg;
    if (arg_node.type == NodeType::Argument) {
        arr_node = arg_node.first_child;
    }

    if (arr_node == NULL_NODE) {
        error("E120", "len() requires an array argument", n.location);
        return TypedValue::void_val();
    }

    const Node& arr = ast_->arena[arr_node];

    // Polymorphic dispatch (PRD prd-pattern-event-arrays §5.4): a DynArray
    // (produced by notes(e)/freqs(e)) carries its length as a runtime signal
    // in `len_buffer` — no PUSH_CONST. Visiting is cached via node_types_, so
    // it is safe; static arrays fall through to the compile-time count below.
    if (arr.type != NodeType::ArrayLit) {
        TypedValue arg_tv = visit(arr_node);
        if (arg_tv.type == ValueType::DynArray && arg_tv.dyn) {
            return cache_and_return(node, TypedValue::signal(arg_tv.dyn->len_buffer));
        }
    }

    // Count elements based on node type
    std::size_t length = 0;

    if (arr.type == NodeType::ArrayLit) {
        // Count children of array literal
        NodeIndex elem = arr.first_child;
        while (elem != NULL_NODE) {
            length++;
            elem = ast_->arena[elem].next_sibling;
        }
    } else if (arr.type == NodeType::Identifier) {
        // Look up the symbol to see if it's a known array
        std::string name = std::string(ctx_->interner->view(arr.as_identifier()));
        auto sym = symbols_->lookup(name);
        if (sym && sym->kind == SymbolKind::Array) {
            length = sym->array.element_count;
        } else {
            error("E141", "len() requires an array, but '" + name + "' is not an array",
                  arr.location);
            return TypedValue::void_val();
        }
    } else {
        error("E122", "len() argument must be an array", arr.location);
        return TypedValue::void_val();
    }

    // Emit the length as a constant
    const std::uint16_t out =
        codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
            .const_value(static_cast<float>(length))
            .emit(*this, n.location);
    if (out == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    return cache_and_return(node, TypedValue::signal(out));
}

// key_deltas(root, intervals) — compile-time quantize table for the
// user-defined key() overload (prd-scale-quantize §4.6). Both arguments
// resolve at compile time: `root` is a note-name string ("d#2" — octave
// accepted and ignored) or a number (pitch class mod 12); `intervals` a
// constant array. Called from the stdlib body of `fn key(events, root,
// ivals)`, so both args are usually fn-param identifiers — resolved back
// to the caller's literal nodes via param_literals_ /
// param_multi_buffer_sources_ (same mechanism as linspace/match-fold).
// The result materializes as a 12-element constant array, exactly the
// shape an equivalent ArrayLit would produce.
TypedValue CodeGenerator::handle_key_deltas_call(NodeIndex node, const Node& n) {
    // Collect the two argument nodes (unwrapping Argument wrappers).
    NodeIndex arg_nodes[2] = {NULL_NODE, NULL_NODE};
    int argc = 0;
    for (NodeIndex child = n.first_child; child != NULL_NODE && argc < 2;
         child = ast_->arena[child].next_sibling, ++argc) {
        NodeIndex a = child;
        if (ast_->arena[a].type == NodeType::Argument) {
            a = ast_->arena[a].first_child;
        }
        arg_nodes[argc] = a;
    }
    if (argc < 2 || arg_nodes[0] == NULL_NODE || arg_nodes[1] == NULL_NODE) {
        error("E120", "key_deltas() requires 2 arguments (root, intervals)",
              n.location);
        return TypedValue::error_val();
    }

    // Root: see through a fn-param identifier to the caller's literal.
    NodeIndex root_node = resolve_param_literal(arg_nodes[0]);
    const Node& root_n = ast_->arena[root_node];
    int root_pc = -1;
    if (root_n.type == NodeType::StringLit) {
        root_pc = note_name_pitch_class(root_n.as_string());
        if (root_pc < 0) {
            error("E203",
                  "key(): root '" + root_n.as_string() +
                      "' is not a note name (expected e.g. \"d\", \"f#\", "
                      "\"a#2\")",
                  n.location);
            return TypedValue::error_val();
        }
    } else {
        ConstEvaluator evaluator(*ast_, *symbols_, *ctx_->interner);
        auto root_val = evaluator.evaluate(root_node);
        if (!root_val || !std::holds_alternative<double>(*root_val)) {
            error("E203",
                  "key(): root must be a note-name string or a compile-time "
                  "constant number",
                  n.location);
            return TypedValue::error_val();
        }
        root_pc =
            ((static_cast<int>(std::get<double>(*root_val)) % 12) + 12) % 12;
    }

    // Intervals: a fn-param identifier resolves to the caller's array node
    // via param_multi_buffer_sources_; a direct ArrayLit const-evals as-is.
    NodeIndex ivals_node = arg_nodes[1];
    const Node& ivals_n = ast_->arena[ivals_node];
    if (ivals_n.type == NodeType::Identifier) {
        std::string name;
        if (std::holds_alternative<Node::IdentifierData>(ivals_n.data)) {
            name = ctx_->interner->view(ivals_n.as_identifier());
        }
        if (!name.empty()) {
            auto it = param_multi_buffer_sources_.find(fnv1a_hash(name));
            if (it != param_multi_buffer_sources_.end()) {
                ivals_node = it->second;
                if (ast_->arena[ivals_node].type == NodeType::Argument) {
                    ivals_node = ast_->arena[ivals_node].first_child;
                }
            }
        }
    }
    ConstEvaluator evaluator(*ast_, *symbols_, *ctx_->interner);
    auto ivals_val = evaluator.evaluate(ivals_node);
    for (const auto& diag : evaluator.diagnostics()) {
        diagnostics_.push_back(diag);
    }
    if (!ivals_val ||
        !std::holds_alternative<std::vector<double>>(*ivals_val)) {
        error("E203",
              "key(): interval list must be a compile-time-constant array",
              n.location);
        return TypedValue::error_val();
    }

    const std::vector<double> deltas = compute_key_deltas(
        root_pc, std::get<std::vector<double>>(*ivals_val));

    std::vector<TypedValue> elements;
    elements.reserve(deltas.size());
    std::uint16_t first_buf = BufferAllocator::BUFFER_UNUSED;
    for (double v : deltas) {
        const std::uint16_t out =
            codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
                .const_value(static_cast<float>(v))
                .emit(*this, n.location);
        if (out == BufferAllocator::BUFFER_UNUSED) {
            return TypedValue::error_val();
        }
        if (first_buf == BufferAllocator::BUFFER_UNUSED) first_buf = out;
        elements.push_back(TypedValue::signal(out));
    }
    return cache_and_return(node,
                            TypedValue::make_array(std::move(elements), first_buf));
}

// Shared codegen for notes()/freqs() and the e.notes/e.freqs field forms.
// Emits SEQPAT_VALUES (packed chord data) + SEQPAT_FIELD/num_values (runtime
// length), wrapped into a DynArray. PRD prd-pattern-event-arrays §5.3.
TypedValue CodeGenerator::emit_pattern_values(NodeIndex node,
                                              NodeIndex pattern_src,
                                              const TypedValue& pattern_tv,
                                              bool to_midi, SourceLocation loc) {
    const char* fn = to_midi ? "notes()" : "freqs()";
    if (pattern_tv.type != ValueType::Pattern || !pattern_tv.pattern) {
        error("E182", std::string(fn) + " expects a pattern argument, got " +
              value_type_name(pattern_tv.type), loc);
        return TypedValue::error_val();
    }

    std::uint32_t state_id = pattern_tv.pattern->state_id;

    // notes()/freqs() turn the chord into data — count as a polyphony
    // consumer (by pattern identity) so a chord-symbol pattern is not
    // flagged by the E410 "chord pattern not wrapped in poly()" check.
    (void)pattern_src;
    consume_polyphonic_pattern(state_id);
    std::uint16_t data_buf = buffers_.allocate();
    std::uint16_t len_buf  = buffers_.allocate();
    if (data_buf == BufferAllocator::BUFFER_UNUSED ||
        len_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", loc);
        return TypedValue::error_val();
    }

    // Packed chord-note data buffer. rate 1 = MIDI (notes), 0 = Hz (freqs).
    // All input slots unused (internal clock).
    codegen::InstructionBuilder(cedar::Opcode::SEQPAT_VALUES)
        .rate(to_midi ? 1 : 0)
        .state_id(state_id)
        .output(data_buf)
        .emit(*this);

    // Runtime length buffer — SEQPAT_FIELD num_values selector (5).
    codegen::InstructionBuilder(cedar::Opcode::SEQPAT_FIELD)
        .rate(5)                // num_values
        .input(0, 0)            // voice 0 (num_values is event-scoped)
        .state_id(state_id)     // inputs[1..4] unused (internal clock)
        .output(len_buf)
        .emit(*this);

    return cache_and_return(node, TypedValue::make_dyn_array(data_buf, len_buf));
}

// notes(e) / freqs(e): pattern-event chord notes as a dynamic array.
static NodeIndex unwrap_len_style_arg(const AstArena& arena, const Node& n) {
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) return NULL_NODE;
    const Node& arg_node = arena[arg];
    return (arg_node.type == NodeType::Argument) ? arg_node.first_child : arg;
}

TypedValue CodeGenerator::handle_notes_call(NodeIndex node, const Node& n) {
    NodeIndex arr_node = unwrap_len_style_arg(ast_->arena, n);
    if (arr_node == NULL_NODE) {
        error("E182", "notes() requires exactly 1 pattern argument", n.location);
        return TypedValue::error_val();
    }
    TypedValue pat = visit(arr_node);
    return emit_pattern_values(node, arr_node, pat, /*to_midi=*/true, n.location);
}

TypedValue CodeGenerator::handle_freqs_call(NodeIndex node, const Node& n) {
    NodeIndex arr_node = unwrap_len_style_arg(ast_->arena, n);
    if (arr_node == NULL_NODE) {
        error("E182", "freqs() requires exactly 1 pattern argument", n.location);
        return TypedValue::error_val();
    }
    TypedValue pat = visit(arr_node);
    return emit_pattern_values(node, arr_node, pat, /*to_midi=*/false, n.location);
}

// ============================================================================
// Binary operation broadcasting
// ============================================================================

// Map function name to opcode
static cedar::Opcode binary_name_to_opcode(std::string_view func_name) {
    if (func_name == "add") return cedar::Opcode::ADD;
    if (func_name == "sub") return cedar::Opcode::SUB;
    if (func_name == "mul") return cedar::Opcode::MUL;
    if (func_name == "div") return cedar::Opcode::DIV;
    if (func_name == "pow") return cedar::Opcode::POW;
    return cedar::Opcode::NOP;
}

// Emit a single binary operation instruction. Routes through CodeGenerator's
// emit() so source_locations_ stays in sync (PRD-correctness Phase 3 / F2).
static std::uint16_t emit_binary_op(
    CodeGenerator& gen,
    BufferAllocator& buffers,
    cedar::Opcode op,
    std::uint16_t lhs,
    std::uint16_t rhs
) {
    std::uint16_t out = buffers.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        return BufferAllocator::BUFFER_UNUSED;
    }

    codegen::InstructionBuilder(op)
        .inputs({lhs, rhs})
        .output(out)
        .emit(gen);

    return out;
}

TypedValue CodeGenerator::handle_binary_op_call(NodeIndex node, const Node& n) {
    std::string func_name;
    if (std::holds_alternative<Node::IdentifierData>(n.data)) {
        func_name = ctx_->interner->view(n.as_identifier());
    } else {
        error("E160", "Invalid binary operation call", n.location);
        return TypedValue::void_val();
    }

    cedar::Opcode op = binary_name_to_opcode(func_name);
    if (op == cedar::Opcode::NOP) {
        error("E161", "Unknown binary operation: " + func_name, n.location);
        return TypedValue::void_val();
    }

    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E162", func_name + "() requires 2 arguments", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t buf_a = visit(args.nodes[0]).buffer;
    std::uint16_t buf_b = visit(args.nodes[1]).buffer;

    bool a_multi = is_multi_buffer(args.nodes[0]);
    bool b_multi = is_multi_buffer(args.nodes[1]);

    // Neither is array - standard scalar operation
    if (!a_multi && !b_multi) {
        std::uint16_t result = emit_binary_op(*this, buffers_, op, buf_a, buf_b);
        return cache_and_return(node, TypedValue::signal(result));
    }

    auto buffers_a = a_multi ? get_multi_buffers(args.nodes[0])
                             : std::vector<std::uint16_t>{buf_a};
    auto buffers_b = b_multi ? get_multi_buffers(args.nodes[1])
                             : std::vector<std::uint16_t>{buf_b};

    // Broadcasting: use the larger size, cycle the smaller array
    std::size_t len = std::max(buffers_a.size(), buffers_b.size());
    std::vector<std::uint16_t> result_buffers;

    for (std::size_t i = 0; i < len; ++i) {
        std::uint16_t a = buffers_a[i % buffers_a.size()];
        std::uint16_t b = buffers_b[i % buffers_b.size()];
        std::uint16_t res = emit_binary_op(*this, buffers_, op, a, b);
        if (res == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }
        result_buffers.push_back(res);
    }

    // PRD §5.3 rule 4 / §10.11: when either operand was Stereo (or a Mono
    // broadcast against one), the 2-buffer broadcast result is a Stereo
    // signal, not a generic Array. Tag it as such so downstream auto-lift
    // and out() treat it correctly. We only do this when: exactly 2 output
    // buffers, the buffers are adjacent (L, L+1), and at least one operand
    // was a stereo signal (as opposed to, say, a chord-expansion Array).
    bool a_stereo = a_multi && is_stereo(args.nodes[0]);
    bool b_stereo = b_multi && is_stereo(args.nodes[1]);
    if (result_buffers.size() == 2 && (a_stereo || b_stereo) &&
        result_buffers[1] == result_buffers[0] + 1) {
        register_stereo(node, result_buffers[0], result_buffers[1]);
        auto tv = TypedValue::stereo_signal(result_buffers[0], result_buffers[1]);
        node_types_[node] = tv;
        return tv;
    }

    return finalize_array_result(node, std::move(result_buffers));
}

// ============================================================================
// Array reduction operations
// ============================================================================

// mean(array) - average of elements
TypedValue CodeGenerator::handle_mean_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1);
    if (!args.valid) {
        error("E164", "mean() requires 1 argument: mean(array)", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t array_buf = visit(args.nodes[0]).buffer;

    if (!is_multi_buffer(args.nodes[0])) {
        // Single element - return as-is
        return cache_and_return(node, TypedValue::signal(array_buf));
    }

    auto elem_bufs = get_multi_buffers(args.nodes[0]);
    if (elem_bufs.empty()) {
        std::uint16_t zero = emit_zero();
        return cache_and_return(node, TypedValue::signal(zero));
    }
    if (elem_bufs.size() == 1) {
        return cache_and_return(node, TypedValue::signal(elem_bufs[0]));
    }

    // Sum all elements with an in-place accumulator (see handle_sum_call
    // for the rationale).
    std::uint16_t sum_buf = alloc_buffer(n.location);
    if (sum_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }
    // Slot 4 stays 0 (historical zero-init encoding, kept for byte-identical
    // bytecode).
    codegen::InstructionBuilder(cedar::Opcode::ADD)
        .inputs({elem_bufs[0], elem_bufs[1], 0xFFFF, 0xFFFF, 0})
        .output(sum_buf)
        .emit(*this);
    for (std::size_t i = 2; i < elem_bufs.size(); ++i) {
        codegen::InstructionBuilder(cedar::Opcode::ADD)
            .inputs({sum_buf, elem_bufs[i], 0xFFFF, 0xFFFF, 0})
            .output(sum_buf)
            .emit(*this);
    }

    // Divide by length
    std::uint16_t len_buf = emit_push_const(static_cast<float>(elem_bufs.size()));
    if (len_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t result = emit_binary_op(*this, buffers_, cedar::Opcode::DIV, sum_buf, len_buf);
    return cache_and_return(node, TypedValue::signal(result));
}

// min/max with array support - either binary or reduction
TypedValue CodeGenerator::handle_minmax_call(NodeIndex node, const Node& n) {
    std::string func_name;
    if (std::holds_alternative<Node::IdentifierData>(n.data)) {
        func_name = ctx_->interner->view(n.as_identifier());
    }

    cedar::Opcode op = (func_name == "min") ? cedar::Opcode::MIN : cedar::Opcode::MAX;

    auto args = extract_call_args(ast_->arena, n.first_child, 1, 2);
    if (!args.valid || args.nodes.empty()) {
        error("E165", func_name + "() requires 1 or 2 arguments", n.location);
        return TypedValue::void_val();
    }

    // Two arguments: standard binary min/max
    if (args.nodes.size() == 2) {
        std::uint16_t buf_a = visit(args.nodes[0]).buffer;
        std::uint16_t buf_b = visit(args.nodes[1]).buffer;
        std::uint16_t result = emit_binary_op(*this, buffers_, op, buf_a, buf_b);
        return cache_and_return(node, TypedValue::signal(result));
    }

    // Single argument: reduction over array
    std::uint16_t array_buf = visit(args.nodes[0]).buffer;

    if (!is_multi_buffer(args.nodes[0])) {
        // Single element - return as-is
        return cache_and_return(node, TypedValue::signal(array_buf));
    }

    auto elem_bufs = get_multi_buffers(args.nodes[0]);
    if (elem_bufs.empty()) {
        // min/max of empty array - return 0
        std::uint16_t zero = emit_zero();
        return cache_and_return(node, TypedValue::signal(zero));
    }
    if (elem_bufs.size() == 1) {
        return cache_and_return(node, TypedValue::signal(elem_bufs[0]));
    }

    std::uint16_t result = elem_bufs[0];
    for (std::size_t i = 1; i < elem_bufs.size(); ++i) {
        result = emit_binary_op(*this, buffers_, op, result, elem_bufs[i]);
        if (result == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }
    }

    return cache_and_return(node, TypedValue::signal(result));
}

// ============================================================================
// Array transformation operations
// ============================================================================

// rotate(array, n) - rotate elements by n positions
TypedValue CodeGenerator::handle_rotate_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E166", "rotate() requires 2 arguments: rotate(array, n)", n.location);
        return TypedValue::void_val();
    }

    // n must be a literal
    const Node& n_val = ast_->arena[args.nodes[1]];
    if (n_val.type != NodeType::NumberLit) {
        error("E167", "rotate() second argument must be a number literal", n_val.location);
        return TypedValue::void_val();
    }

    int offset = static_cast<int>(n_val.as_number());
    std::uint16_t array_buf = visit(args.nodes[0]).buffer;

    if (!is_multi_buffer(args.nodes[0])) {
        // Single element - return as-is
        return cache_and_return(node, TypedValue::signal(array_buf));
    }

    auto elem_bufs = get_multi_buffers(args.nodes[0]);
    if (elem_bufs.empty() || elem_bufs.size() == 1) {
        std::uint16_t first_buf = elem_bufs.empty() ? emit_zero() : elem_bufs[0];
        return cache_and_return(node, TypedValue::signal(first_buf));
    }

    int len = static_cast<int>(elem_bufs.size());
    // Normalize offset (positive = rotate right, negative = rotate left)
    offset = ((offset % len) + len) % len;

    std::vector<std::uint16_t> result;
    for (int i = 0; i < len; ++i) {
        // rotate right by offset: element at i comes from (i - offset + len) % len
        int src_idx = (i - offset + len) % len;
        result.push_back(elem_bufs[static_cast<std::size_t>(src_idx)]);
    }

    return finalize_array_result(node, std::move(result));
}

// shuffle(array) - deterministic random permutation
TypedValue CodeGenerator::handle_shuffle_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1, 2);
    if (!args.valid) {
        error("E168", "shuffle() requires 1 or 2 arguments: shuffle(array, seed=auto)", n.location);
        return TypedValue::void_val();
    }

    std::optional<double> user_seed;
    if (args.nodes.size() == 2) {
        user_seed = resolve_const_scalar(ast_->arena, args.nodes[1], *symbols_);
        if (!user_seed) {
            error("E168", "shuffle() seed must be a compile-time constant",
                  ast_->arena[args.nodes[1]].location);
            return TypedValue::void_val();
        }
    }

    std::uint16_t array_buf = visit(args.nodes[0]).buffer;

    if (!is_multi_buffer(args.nodes[0])) {
        return cache_and_return(node, TypedValue::signal(array_buf));
    }

    auto elem_bufs = get_multi_buffers(args.nodes[0]);
    if (elem_bufs.size() <= 1) {
        std::uint16_t first_buf = elem_bufs.empty() ? emit_zero() : elem_bufs[0];
        return cache_and_return(node, TypedValue::signal(first_buf));
    }

    // Default seed: semantic path hash for deterministic shuffle.
    // User-provided seed XORs in so the same code position can produce
    // distinct permutations.
    std::uint32_t seed = compute_state_id();
    if (user_seed) {
        seed ^= static_cast<std::uint32_t>(static_cast<std::int64_t>(*user_seed));
    }

    // Fisher-Yates shuffle with simple LCG
    std::vector<std::uint16_t> result = elem_bufs;
    for (std::size_t i = result.size() - 1; i > 0; --i) {
        seed = seed * 1103515245 + 12345;  // Simple LCG
        std::size_t j = (seed >> 16) % (i + 1);
        std::swap(result[i], result[j]);
    }

    return finalize_array_result(node, std::move(result));
}

// sort(array, reverse=false) - sort elements (compile-time constants only)
TypedValue CodeGenerator::handle_sort_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1, 2);
    if (!args.valid) {
        error("E169", "sort() requires 1 or 2 arguments: sort(array, reverse=false)", n.location);
        return TypedValue::void_val();
    }

    bool reverse = false;
    if (args.nodes.size() == 2) {
        auto rev_val = resolve_const_scalar(ast_->arena, args.nodes[1], *symbols_);
        if (!rev_val) {
            error("E169", "sort() reverse argument must be a compile-time constant",
                  ast_->arena[args.nodes[1]].location);
            return TypedValue::void_val();
        }
        reverse = (*rev_val != 0.0);
    }

    // For compile-time sort, we need to extract literal values
    // This only works for array literals with number elements
    const Node& arr_node = ast_->arena[args.nodes[0]];
    if (arr_node.type != NodeType::ArrayLit) {
        // For non-literals, just pass through (can't sort at compile time)
        std::uint16_t array_buf = visit(args.nodes[0]).buffer;
        return cache_and_return(node, TypedValue::signal(array_buf));
    }

    // Extract literal values
    std::vector<std::pair<float, NodeIndex>> values;
    NodeIndex elem = arr_node.first_child;
    while (elem != NULL_NODE) {
        const Node& elem_node = ast_->arena[elem];
        if (elem_node.type == NodeType::NumberLit) {
            values.push_back({static_cast<float>(elem_node.as_number()), elem});
        } else {
            // Non-literal element - can't sort at compile time
            std::uint16_t array_buf = visit(args.nodes[0]).buffer;
            return cache_and_return(node, TypedValue::signal(array_buf));
        }
        elem = ast_->arena[elem].next_sibling;
    }

    if (values.empty()) {
        std::uint16_t zero = emit_zero();
        return cache_and_return(node, TypedValue::signal(zero));
    }

    // Sort by value (reverse swaps comparator)
    if (reverse) {
        std::sort(values.begin(), values.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
    } else {
        std::sort(values.begin(), values.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    // Emit sorted constants
    std::vector<std::uint16_t> result_buffers;
    for (const auto& [val, _] : values) {
        std::uint16_t buf = emit_push_const(val);
        if (buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }
        result_buffers.push_back(buf);
    }

    return finalize_array_result(node, std::move(result_buffers));
}

// normalize(array, lo=0, hi=1) - scale to [lo, hi] range
TypedValue CodeGenerator::handle_normalize_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1, 3);
    if (!args.valid) {
        error("E170", "normalize() requires 1-3 arguments: normalize(array, lo=0, hi=1)", n.location);
        return TypedValue::void_val();
    }

    double lo_val = 0.0;
    double hi_val = 1.0;
    if (args.nodes.size() >= 2) {
        auto v = resolve_const_scalar(ast_->arena, args.nodes[1], *symbols_);
        if (!v) {
            error("E170", "normalize() lo must be a compile-time constant",
                  ast_->arena[args.nodes[1]].location);
            return TypedValue::void_val();
        }
        lo_val = *v;
    }
    if (args.nodes.size() == 3) {
        auto v = resolve_const_scalar(ast_->arena, args.nodes[2], *symbols_);
        if (!v) {
            error("E170", "normalize() hi must be a compile-time constant",
                  ast_->arena[args.nodes[2]].location);
            return TypedValue::void_val();
        }
        hi_val = *v;
    }
    if (hi_val <= lo_val) {
        error("E170", "normalize() requires hi > lo", n.location);
        return TypedValue::void_val();
    }

    std::uint16_t array_buf = visit(args.nodes[0]).buffer;

    if (!is_multi_buffer(args.nodes[0])) {
        // Single element normalizes to lo (constant target floor)
        std::uint16_t out = emit_push_const(static_cast<float>(lo_val));
        return cache_and_return(node, TypedValue::signal(out));
    }

    auto elem_bufs = get_multi_buffers(args.nodes[0]);
    if (elem_bufs.size() <= 1) {
        std::uint16_t out = emit_push_const(static_cast<float>(lo_val));
        return cache_and_return(node, TypedValue::signal(out));
    }

    // Find min
    std::uint16_t min_buf = elem_bufs[0];
    for (std::size_t i = 1; i < elem_bufs.size(); ++i) {
        min_buf = emit_binary_op(*this, buffers_, cedar::Opcode::MIN, min_buf, elem_bufs[i]);
    }

    // Find max
    std::uint16_t max_buf = elem_bufs[0];
    for (std::size_t i = 1; i < elem_bufs.size(); ++i) {
        max_buf = emit_binary_op(*this, buffers_, cedar::Opcode::MAX, max_buf, elem_bufs[i]);
    }

    // src_range = max - min
    std::uint16_t src_range = emit_binary_op(*this, buffers_, cedar::Opcode::SUB, max_buf, min_buf);

    // For the default [0,1] path, skip the extra MUL/ADD to keep emitted
    // ops identical to the pre-extension behavior.
    bool default_target = (lo_val == 0.0 && hi_val == 1.0);
    std::uint16_t span_buf = 0;
    std::uint16_t lo_buf = 0;
    if (!default_target) {
        span_buf = emit_push_const(static_cast<float>(hi_val - lo_val));
        lo_buf = emit_push_const(static_cast<float>(lo_val));
    }

    std::vector<std::uint16_t> result_buffers;
    for (auto buf : elem_bufs) {
        std::uint16_t shifted = emit_binary_op(*this, buffers_, cedar::Opcode::SUB, buf, min_buf);
        std::uint16_t unit = emit_binary_op(*this, buffers_, cedar::Opcode::DIV, shifted, src_range);
        std::uint16_t out;
        if (default_target) {
            out = unit;
        } else {
            std::uint16_t scaled = emit_binary_op(*this, buffers_, cedar::Opcode::MUL, unit, span_buf);
            out = emit_binary_op(*this, buffers_, cedar::Opcode::ADD, scaled, lo_buf);
        }
        result_buffers.push_back(out);
    }

    return finalize_array_result(node, std::move(result_buffers));
}


// ============================================================================
// Array generation operations
// ============================================================================

// linspace(start, end, n, mode="linear") - N evenly spaced values
TypedValue CodeGenerator::handle_linspace_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 3, 4);
    if (!args.valid) {
        error("E172", "linspace() requires 3 or 4 arguments: linspace(start, end, n, mode=\"linear\")",
              n.location);
        return TypedValue::void_val();
    }

    // start/end/n must be compile-time constants
    auto start_val = resolve_const_scalar(ast_->arena, resolve_param_literal(args.nodes[0]), *symbols_);
    auto end_val = resolve_const_scalar(ast_->arena, resolve_param_literal(args.nodes[1]), *symbols_);
    auto n_val = resolve_const_scalar(ast_->arena, resolve_param_literal(args.nodes[2]), *symbols_);

    if (!start_val || !end_val || !n_val) {
        error("E173", "linspace() arguments must be compile-time constants", n.location);
        return TypedValue::void_val();
    }

    enum class Mode { Linear, Log, Geom };
    Mode mode = Mode::Linear;
    if (args.nodes.size() == 4) {
        const Node& mode_node = ast_->arena[args.nodes[3]];
        if (mode_node.type != NodeType::StringLit) {
            error("E173", "linspace() mode must be a string literal", mode_node.location);
            return TypedValue::void_val();
        }
        std::string m = std::string(mode_node.as_string());
        if (m == "linear")    mode = Mode::Linear;
        else if (m == "log")  mode = Mode::Log;
        else if (m == "geom") mode = Mode::Geom;
        else {
            error("E173", "linspace() mode must be \"linear\", \"log\", or \"geom\"",
                  mode_node.location);
            return TypedValue::void_val();
        }
    }

    double start = *start_val;
    double end = *end_val;
    int count = static_cast<int>(*n_val);

    if (count <= 0) {
        std::uint16_t zero = emit_zero();
        return cache_and_return(node, TypedValue::signal(zero));
    }

    if ((mode == Mode::Log || mode == Mode::Geom) && (start <= 0.0 || end <= 0.0)) {
        error("E173", "linspace() log/geom modes require start > 0 and end > 0", n.location);
        return TypedValue::void_val();
    }

    std::vector<std::uint16_t> result_buffers;
    for (int i = 0; i < count; ++i) {
        double val;
        if (count == 1) {
            val = start;
        } else if (mode == Mode::Linear) {
            double t = static_cast<double>(i) / static_cast<double>(count - 1);
            val = start + t * (end - start);
        } else {
            // log and geom both produce the same numbers (geometric series),
            // exposed as two names to match user intent (frequency sweep vs
            // exponential ramp).
            double t = static_cast<double>(i) / static_cast<double>(count - 1);
            val = start * std::pow(end / start, t);
        }
        std::uint16_t buf = emit_push_const(static_cast<float>(val));
        if (buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }
        result_buffers.push_back(buf);
    }

    return finalize_array_result(node, std::move(result_buffers));
}

// random(n, min=0, max=1) - N random values in [min, max) (deterministic by path)
TypedValue CodeGenerator::handle_random_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1, 3);
    if (!args.valid) {
        error("E174", "random() requires 1-3 arguments: random(n, min=0, max=1)", n.location);
        return TypedValue::void_val();
    }

    auto n_val = resolve_const_scalar(ast_->arena, args.nodes[0], *symbols_);
    if (!n_val) {
        error("E175", "random() argument must be a compile-time constant",
              ast_->arena[args.nodes[0]].location);
        return TypedValue::void_val();
    }

    double min_val = 0.0;
    double max_val = 1.0;
    if (args.nodes.size() >= 2) {
        auto m = resolve_const_scalar(ast_->arena, args.nodes[1], *symbols_);
        if (!m) {
            error("E175", "random() min must be a compile-time constant",
                  ast_->arena[args.nodes[1]].location);
            return TypedValue::void_val();
        }
        min_val = *m;
    }
    if (args.nodes.size() == 3) {
        auto m = resolve_const_scalar(ast_->arena, args.nodes[2], *symbols_);
        if (!m) {
            error("E175", "random() max must be a compile-time constant",
                  ast_->arena[args.nodes[2]].location);
            return TypedValue::void_val();
        }
        max_val = *m;
    }

    if (max_val <= min_val) {
        error("E175", "random() requires max > min", n.location);
        return TypedValue::void_val();
    }

    int count = static_cast<int>(*n_val);
    if (count <= 0) {
        std::uint16_t zero = emit_zero();
        return cache_and_return(node, TypedValue::signal(zero));
    }

    // Use semantic path hash as seed for deterministic random values
    std::uint32_t seed = compute_state_id();
    float span = static_cast<float>(max_val - min_val);
    float lo = static_cast<float>(min_val);

    std::vector<std::uint16_t> result_buffers;
    for (int i = 0; i < count; ++i) {
        // Simple LCG to generate random float in [0, 1), then rescale.
        seed = seed * 1103515245 + 12345;
        float u = static_cast<float>((seed >> 16) & 0x7FFF) / 32768.0f;
        float val = lo + u * span;

        std::uint16_t buf = emit_push_const(val);
        if (buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }
        result_buffers.push_back(buf);
    }

    return finalize_array_result(node, std::move(result_buffers));
}

// harmonics(fundamental, n, ratio=1.0) - harmonic series with optional inharmonicity
TypedValue CodeGenerator::handle_harmonics_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2, 3);
    if (!args.valid) {
        error("E176", "harmonics() requires 2 or 3 arguments: harmonics(fundamental, n, ratio=1)", n.location);
        return TypedValue::void_val();
    }

    auto fund_val = resolve_const_scalar(ast_->arena, args.nodes[0], *symbols_);
    auto n_val = resolve_const_scalar(ast_->arena, args.nodes[1], *symbols_);

    if (!fund_val || !n_val) {
        error("E177", "harmonics() arguments must be compile-time constants", n.location);
        return TypedValue::void_val();
    }

    double ratio_val = 1.0;
    if (args.nodes.size() == 3) {
        auto r = resolve_const_scalar(ast_->arena, args.nodes[2], *symbols_);
        if (!r) {
            error("E177", "harmonics() ratio must be a compile-time constant",
                  ast_->arena[args.nodes[2]].location);
            return TypedValue::void_val();
        }
        ratio_val = *r;
    }

    if (ratio_val <= -1.0) {
        error("E177", "harmonics() ratio must be > -1 (1 + ratio must be positive)", n.location);
        return TypedValue::void_val();
    }

    float fundamental = static_cast<float>(*fund_val);
    int count = static_cast<int>(*n_val);

    if (count <= 0) {
        std::uint16_t zero = emit_zero();
        return cache_and_return(node, TypedValue::signal(zero));
    }

    // Default ratio==1 keeps the integer-multiplier fast path to avoid
    // float drift on the most common case.
    bool natural = (ratio_val == 1.0);
    double exponent = natural ? 0.0 : std::log2(1.0 + ratio_val);

    std::vector<std::uint16_t> result_buffers;
    for (int i = 1; i <= count; ++i) {
        float harmonic = natural
            ? fundamental * static_cast<float>(i)
            : fundamental * static_cast<float>(std::pow(static_cast<double>(i), exponent));
        std::uint16_t buf = emit_push_const(harmonic);
        if (buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }
        result_buffers.push_back(buf);
    }

    return finalize_array_result(node, std::move(result_buffers));
}

} // namespace akkado
