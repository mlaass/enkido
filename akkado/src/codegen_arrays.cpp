// Array higher-order function codegen implementations
// Extracted from codegen.cpp for maintainability

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include <algorithm>

namespace akkado {

using codegen::encode_const_value;
using codegen::unwrap_argument;
using codegen::emit_zero;
using codegen::extract_call_args;
using codegen::get_input_buffers;

// Multi-buffer registration
std::uint16_t CodeGenerator::register_multi_buffer(NodeIndex node, std::vector<std::uint16_t> buffers) {
    if (buffers.empty()) return BufferAllocator::BUFFER_UNUSED;
    multi_buffers_[node] = std::move(buffers);
    return multi_buffers_[node][0];
}

bool CodeGenerator::is_multi_buffer(NodeIndex node) const {
    auto it = multi_buffers_.find(node);
    return it != multi_buffers_.end() && it->second.size() > 1;
}

std::vector<std::uint16_t> CodeGenerator::get_multi_buffers(NodeIndex node) const {
    auto it = multi_buffers_.find(node);
    if (it != multi_buffers_.end()) return it->second;

    auto buf_it = node_buffers_.find(node);
    if (buf_it != node_buffers_.end() && buf_it->second != BufferAllocator::BUFFER_UNUSED) {
        return {buf_it->second};
    }
    return {};
}

// Apply lambda to single buffer
std::uint16_t CodeGenerator::apply_lambda(NodeIndex lambda_node, std::uint16_t arg_buf) {
    const Node& lambda = ast_->arena[lambda_node];
    if (lambda.type != NodeType::Closure) {
        error("E130", "map() second argument must be a lambda (fn => expr)", lambda.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::vector<std::string> param_names;
    NodeIndex child = lambda.first_child;
    NodeIndex body = NULL_NODE;

    while (child != NULL_NODE) {
        const Node& child_node = ast_->arena[child];
        if (child_node.type == NodeType::Identifier) {
            if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                param_names.push_back(child_node.as_closure_param().name);
            } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                param_names.push_back(child_node.as_identifier());
            } else {
                body = child;
                break;
            }
        } else {
            body = child;
            break;
        }
        child = ast_->arena[child].next_sibling;
    }

    if (body == NULL_NODE) {
        error("E131", "Lambda has no body", lambda.location);
        return BufferAllocator::BUFFER_UNUSED;
    }
    if (param_names.empty()) {
        error("E132", "Lambda must have at least one parameter", lambda.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    symbols_->push_scope();
    symbols_->define_variable(param_names[0], arg_buf);

    auto saved_node_buffers = std::move(node_buffers_);
    node_buffers_.clear();

    std::uint16_t result = visit(body);

    node_buffers_ = std::move(saved_node_buffers);
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

        NodeIndex child = n.first_child;
        while (child != NULL_NODE) {
            const Node& child_node = ast_->arena[child];
            if (child_node.type == NodeType::Identifier) {
                FunctionParamInfo param;
                if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                    param.name = child_node.as_closure_param().name;
                    param.default_value = child_node.as_closure_param().default_value;
                } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                    param.name = child_node.as_identifier();
                    param.default_value = std::nullopt;
                } else {
                    break;
                }
                ref.params.push_back(std::move(param));
            } else {
                break;
            }
            child = ast_->arena[child].next_sibling;
        }
        return ref;
    }

    if (n.type == NodeType::Identifier) {
        std::string name;
        if (std::holds_alternative<Node::IdentifierData>(n.data)) {
            name = n.as_identifier();
        } else {
            return std::nullopt;
        }

        auto sym = symbols_->lookup(name);
        if (!sym) return std::nullopt;

        if (sym->kind == SymbolKind::FunctionValue) {
            return sym->function_ref;
        }
        if (sym->kind == SymbolKind::UserFunction) {
            FunctionRef ref{};
            ref.is_user_function = true;
            ref.user_function_name = sym->name;
            ref.params = sym->user_function.params;
            ref.closure_node = sym->user_function.body_node;
            return ref;
        }
    }

    return std::nullopt;
}

// Apply unary function ref
std::uint16_t CodeGenerator::apply_function_ref(const FunctionRef& ref, std::uint16_t arg_buf,
                                                  SourceLocation loc) {
    if (ref.params.empty()) {
        error("E132", "Function must have at least one parameter", loc);
        return BufferAllocator::BUFFER_UNUSED;
    }

    symbols_->push_scope();
    for (const auto& capture : ref.captures) {
        symbols_->define_variable(capture.name, capture.buffer_index);
    }
    symbols_->define_variable(ref.params[0].name, arg_buf);

    auto saved_node_buffers = std::move(node_buffers_);
    node_buffers_.clear();

    std::uint16_t result = BufferAllocator::BUFFER_UNUSED;

    if (ref.is_user_function) {
        if (ref.closure_node != NULL_NODE) result = visit(ref.closure_node);
    } else {
        const Node& closure = ast_->arena[ref.closure_node];
        NodeIndex child = closure.first_child;
        NodeIndex body = NULL_NODE;

        while (child != NULL_NODE) {
            const Node& child_node = ast_->arena[child];
            if (child_node.type == NodeType::Identifier &&
                (std::holds_alternative<Node::ClosureParamData>(child_node.data) ||
                 std::holds_alternative<Node::IdentifierData>(child_node.data))) {
                child = child_node.next_sibling;
                continue;
            }
            body = child;
            break;
        }
        if (body != NULL_NODE) result = visit(body);
    }

    node_buffers_ = std::move(saved_node_buffers);
    symbols_->pop_scope();

    return result;
}

// Apply binary function ref
std::uint16_t CodeGenerator::apply_binary_function_ref(const FunctionRef& ref,
                                                        std::uint16_t arg_buf1,
                                                        std::uint16_t arg_buf2,
                                                        SourceLocation loc) {
    if (ref.params.size() < 2) {
        error("E140", "Binary function must have at least two parameters", loc);
        return BufferAllocator::BUFFER_UNUSED;
    }

    symbols_->push_scope();
    for (const auto& capture : ref.captures) {
        symbols_->define_variable(capture.name, capture.buffer_index);
    }
    symbols_->define_variable(ref.params[0].name, arg_buf1);
    symbols_->define_variable(ref.params[1].name, arg_buf2);

    auto saved_node_buffers = std::move(node_buffers_);
    node_buffers_.clear();

    std::uint16_t result = BufferAllocator::BUFFER_UNUSED;

    if (ref.is_user_function) {
        if (ref.closure_node != NULL_NODE) result = visit(ref.closure_node);
    } else {
        const Node& closure = ast_->arena[ref.closure_node];
        NodeIndex child = closure.first_child;
        NodeIndex body = NULL_NODE;

        while (child != NULL_NODE) {
            const Node& child_node = ast_->arena[child];
            if (child_node.type == NodeType::Identifier &&
                (std::holds_alternative<Node::ClosureParamData>(child_node.data) ||
                 std::holds_alternative<Node::IdentifierData>(child_node.data))) {
                child = child_node.next_sibling;
                continue;
            }
            body = child;
            break;
        }
        if (body != NULL_NODE) result = visit(body);
    }

    node_buffers_ = std::move(saved_node_buffers);
    symbols_->pop_scope();

    return result;
}

// Finalize array result (helper)
static std::uint16_t finalize_result(
    CodeGenerator* cg,
    BufferAllocator& buffers,
    std::vector<cedar::Instruction>& instructions,
    NodeIndex node,
    std::vector<std::uint16_t> result_buffers,
    std::unordered_map<NodeIndex, std::uint16_t>& node_buffers,
    std::unordered_map<NodeIndex, std::vector<std::uint16_t>>& multi_buffers
) {
    if (result_buffers.empty()) {
        std::uint16_t zero = emit_zero(buffers, instructions);
        node_buffers[node] = zero;
        return zero;
    }
    if (result_buffers.size() == 1) {
        node_buffers[node] = result_buffers[0];
        return result_buffers[0];
    }
    std::uint16_t first_buf = result_buffers[0];
    multi_buffers[node] = std::move(result_buffers);
    node_buffers[node] = first_buf;
    return first_buf;
}

// map(array, fn)
std::uint16_t CodeGenerator::handle_map_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E133", "map() requires 2 arguments: map(array, fn)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    auto func_ref = resolve_function_arg(args.nodes[1]);
    if (!func_ref) {
        error("E130", "map() second argument must be a function", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t array_buf = visit(args.nodes[0]);

    if (!is_multi_buffer(args.nodes[0])) {
        push_path("map#" + std::to_string(call_counters_["map"]++));
        push_path("elem0");
        std::uint16_t result = apply_function_ref(*func_ref, array_buf, n.location);
        pop_path();
        pop_path();
        node_buffers_[node] = result;
        return result;
    }

    auto element_buffers = get_multi_buffers(args.nodes[0]);
    std::vector<std::uint16_t> result_buffers;

    push_path("map#" + std::to_string(call_counters_["map"]++));
    for (std::size_t i = 0; i < element_buffers.size(); ++i) {
        push_path("elem" + std::to_string(i));
        result_buffers.push_back(apply_function_ref(*func_ref, element_buffers[i], n.location));
        pop_path();
    }
    pop_path();

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// sum(array)
std::uint16_t CodeGenerator::handle_sum_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1);
    if (!args.valid) {
        error("E134", "sum() requires 1 argument: sum(array)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t array_buf = visit(args.nodes[0]);

    if (!is_multi_buffer(args.nodes[0])) {
        node_buffers_[node] = array_buf;
        return array_buf;
    }

    auto buffers = get_multi_buffers(args.nodes[0]);
    if (buffers.empty()) {
        std::uint16_t zero = emit_zero(buffers_, instructions_);
        node_buffers_[node] = zero;
        return zero;
    }
    if (buffers.size() == 1) {
        node_buffers_[node] = buffers[0];
        return buffers[0];
    }

    std::uint16_t result = buffers[0];
    for (std::size_t i = 1; i < buffers.size(); ++i) {
        std::uint16_t sum_buf = buffers_.allocate();
        if (sum_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }

        cedar::Instruction add_inst{};
        add_inst.opcode = cedar::Opcode::ADD;
        add_inst.out_buffer = sum_buf;
        add_inst.inputs[0] = result;
        add_inst.inputs[1] = buffers[i];
        add_inst.inputs[2] = 0xFFFF;
        add_inst.inputs[3] = 0xFFFF;
        add_inst.state_id = 0;
        emit(add_inst);

        result = sum_buf;
    }

    node_buffers_[node] = result;
    return result;
}

// fold(array, fn, init)
std::uint16_t CodeGenerator::handle_fold_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 3);
    if (!args.valid) {
        error("E142", "fold() requires 3 arguments: fold(array, fn, init)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    auto func_ref = resolve_function_arg(args.nodes[1]);
    if (!func_ref) {
        error("E143", "fold() second argument must be a binary function", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t array_buf = visit(args.nodes[0]);
    std::uint16_t init_buf = visit(args.nodes[2]);

    auto buffers = is_multi_buffer(args.nodes[0]) ?
        get_multi_buffers(args.nodes[0]) : std::vector<std::uint16_t>{array_buf};

    if (buffers.empty()) {
        node_buffers_[node] = init_buf;
        return init_buf;
    }

    push_path("fold#" + std::to_string(call_counters_["fold"]++));
    std::uint16_t result = init_buf;
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        push_path("step" + std::to_string(i));
        result = apply_binary_function_ref(*func_ref, result, buffers[i], n.location);
        pop_path();
    }
    pop_path();

    node_buffers_[node] = result;
    return result;
}

// zipWith(a, b, fn)
std::uint16_t CodeGenerator::handle_zipWith_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 3);
    if (!args.valid) {
        error("E144", "zipWith() requires 3 arguments: zipWith(a, b, fn)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    auto func_ref = resolve_function_arg(args.nodes[2]);
    if (!func_ref) {
        error("E145", "zipWith() third argument must be a binary function", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t buf_a = visit(args.nodes[0]);
    std::uint16_t buf_b = visit(args.nodes[1]);

    auto buffers_a = is_multi_buffer(args.nodes[0]) ?
        get_multi_buffers(args.nodes[0]) : std::vector<std::uint16_t>{buf_a};
    auto buffers_b = is_multi_buffer(args.nodes[1]) ?
        get_multi_buffers(args.nodes[1]) : std::vector<std::uint16_t>{buf_b};

    std::size_t len = std::min(buffers_a.size(), buffers_b.size());
    if (len == 0) {
        std::uint16_t zero = emit_zero(buffers_, instructions_);
        node_buffers_[node] = zero;
        return zero;
    }

    push_path("zipWith#" + std::to_string(call_counters_["zipWith"]++));
    std::vector<std::uint16_t> result_buffers;
    for (std::size_t i = 0; i < len; ++i) {
        push_path("elem" + std::to_string(i));
        result_buffers.push_back(apply_binary_function_ref(*func_ref, buffers_a[i], buffers_b[i], n.location));
        pop_path();
    }
    pop_path();

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// zip(a, b) - interleave
std::uint16_t CodeGenerator::handle_zip_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E146", "zip() requires 2 arguments: zip(a, b)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t buf_a = visit(args.nodes[0]);
    std::uint16_t buf_b = visit(args.nodes[1]);

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

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// take(n, array)
std::uint16_t CodeGenerator::handle_take_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E147", "take() requires 2 arguments: take(n, array)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& n_val = ast_->arena[args.nodes[0]];
    if (n_val.type != NodeType::NumberLit) {
        error("E148", "take() first argument must be a number literal", n_val.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::size_t count = static_cast<std::size_t>(n_val.as_number());
    std::uint16_t array_buf = visit(args.nodes[1]);

    auto buffers = is_multi_buffer(args.nodes[1]) ?
        get_multi_buffers(args.nodes[1]) : std::vector<std::uint16_t>{array_buf};

    count = std::min(count, buffers.size());
    std::vector<std::uint16_t> result_buffers(buffers.begin(), buffers.begin() + count);

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// drop(n, array)
std::uint16_t CodeGenerator::handle_drop_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E149", "drop() requires 2 arguments: drop(n, array)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& n_val = ast_->arena[args.nodes[0]];
    if (n_val.type != NodeType::NumberLit) {
        error("E150", "drop() first argument must be a number literal", n_val.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::size_t count = static_cast<std::size_t>(n_val.as_number());
    std::uint16_t array_buf = visit(args.nodes[1]);

    auto buffers = is_multi_buffer(args.nodes[1]) ?
        get_multi_buffers(args.nodes[1]) : std::vector<std::uint16_t>{array_buf};

    count = std::min(count, buffers.size());
    std::vector<std::uint16_t> result_buffers(buffers.begin() + count, buffers.end());

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// reverse(array)
std::uint16_t CodeGenerator::handle_reverse_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 1);
    if (!args.valid) {
        error("E151", "reverse() requires 1 argument: reverse(array)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::uint16_t array_buf = visit(args.nodes[0]);

    if (!is_multi_buffer(args.nodes[0])) {
        node_buffers_[node] = array_buf;
        return array_buf;
    }

    auto buffers = get_multi_buffers(args.nodes[0]);
    std::reverse(buffers.begin(), buffers.end());

    std::uint16_t first_buf = register_multi_buffer(node, std::move(buffers));
    node_buffers_[node] = first_buf;
    return first_buf;
}

// range(start, end)
std::uint16_t CodeGenerator::handle_range_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E152", "range() requires 2 arguments: range(start, end)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& start_val = ast_->arena[args.nodes[0]];
    const Node& end_val = ast_->arena[args.nodes[1]];

    if (start_val.type != NodeType::NumberLit || end_val.type != NodeType::NumberLit) {
        error("E153", "range() arguments must be number literals", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    int start = static_cast<int>(start_val.as_number());
    int end = static_cast<int>(end_val.as_number());

    std::vector<std::uint16_t> result_buffers;
    int step = (start <= end) ? 1 : -1;

    for (int i = start; i != end; i += step) {
        std::uint16_t buf = buffers_.allocate();
        if (buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return BufferAllocator::BUFFER_UNUSED;
        }

        cedar::Instruction inst{};
        inst.opcode = cedar::Opcode::PUSH_CONST;
        inst.out_buffer = buf;
        inst.inputs[0] = 0xFFFF;
        inst.inputs[1] = 0xFFFF;
        inst.inputs[2] = 0xFFFF;
        inst.inputs[3] = 0xFFFF;
        encode_const_value(inst, static_cast<float>(i));
        emit(inst);

        result_buffers.push_back(buf);
    }

    return finalize_result(this, buffers_, instructions_, node, std::move(result_buffers),
                           node_buffers_, multi_buffers_);
}

// repeat(value, n)
std::uint16_t CodeGenerator::handle_repeat_call(NodeIndex node, const Node& n) {
    auto args = extract_call_args(ast_->arena, n.first_child, 2);
    if (!args.valid) {
        error("E154", "repeat() requires 2 arguments: repeat(value, n)", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    const Node& n_val = ast_->arena[args.nodes[1]];
    if (n_val.type != NodeType::NumberLit) {
        error("E155", "repeat() second argument must be a number literal", n_val.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::size_t count = static_cast<std::size_t>(n_val.as_number());
    std::uint16_t value_buf = visit(args.nodes[0]);

    if (count == 0) {
        std::uint16_t zero = emit_zero(buffers_, instructions_);
        node_buffers_[node] = zero;
        return zero;
    }
    if (count == 1) {
        node_buffers_[node] = value_buf;
        return value_buf;
    }

    std::vector<std::uint16_t> result_buffers(count, value_buf);
    std::uint16_t first_buf = register_multi_buffer(node, std::move(result_buffers));
    node_buffers_[node] = first_buf;
    return first_buf;
}

} // namespace akkado
