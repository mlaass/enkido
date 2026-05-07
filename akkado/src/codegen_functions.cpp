// User-defined function and match expression codegen implementations
// Extracted from codegen.cpp for maintainability

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/const_eval.hpp"
#include <cstring>

namespace akkado {

using codegen::encode_const_value;
using codegen::emit_push_const;

// Try to resolve an AST node to a compile-time constant value.
// Returns nullopt if the node cannot be resolved at compile time.
static std::optional<ConstValue> resolve_const_value(
    const AstArena& arena, NodeIndex node, const SymbolTable& symbols) {

    if (node == NULL_NODE) return std::nullopt;

    const Node& n = arena[node];

    switch (n.type) {
        case NodeType::NumberLit:
            return ConstValue{n.as_number()};

        case NodeType::BoolLit:
            return ConstValue{n.as_bool() ? 1.0 : 0.0};

        case NodeType::PitchLit: {
            double midi = static_cast<double>(n.as_pitch());
            double hz = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
            return ConstValue{hz};
        }

        case NodeType::Identifier: {
            std::string name;
            if (std::holds_alternative<Node::IdentifierData>(n.data)) {
                name = n.as_identifier();
            }
            if (name.empty()) return std::nullopt;

            auto sym = symbols.lookup(name);
            if (sym && sym->is_const && sym->const_value.has_value()) {
                return *sym->const_value;
            }
            return std::nullopt;
        }

        case NodeType::ArrayLit: {
            std::vector<double> elements;
            NodeIndex child = n.first_child;
            while (child != NULL_NODE) {
                auto val = resolve_const_value(arena, child, symbols);
                if (!val) return std::nullopt;
                if (!std::holds_alternative<double>(*val)) return std::nullopt;
                elements.push_back(std::get<double>(*val));
                child = arena[child].next_sibling;
            }
            return ConstValue{std::move(elements)};
        }

        default:
            return std::nullopt;
    }
}

// User function call handler - inlines function bodies at call sites
TypedValue CodeGenerator::handle_user_function_call(
    NodeIndex node, const Node& n, const UserFunctionInfo& func) {

    // const fn: try compile-time evaluation if all args are const
    if (func.is_const) {
        std::vector<ConstValue> const_args;
        bool all_const = true;

        NodeIndex arg = n.first_child;
        while (arg != NULL_NODE) {
            NodeIndex arg_value = arg;
            const Node& arg_node = ast_->arena[arg];
            if (arg_node.type == NodeType::Argument) {
                arg_value = arg_node.first_child;
            }

            auto cv = resolve_const_value(ast_->arena, arg_value, *symbols_);
            if (!cv) {
                all_const = false;
                break;
            }
            const_args.push_back(std::move(*cv));
            arg = ast_->arena[arg].next_sibling;
        }

        if (all_const) {
            ConstEvaluator evaluator(*ast_, *symbols_);
            auto result = evaluator.eval_const_fn_call(func, const_args, n.location);

            for (const auto& diag : evaluator.diagnostics()) {
                diagnostics_.push_back(diag);
            }

            if (result) {
                if (std::holds_alternative<double>(*result)) {
                    float val = static_cast<float>(std::get<double>(*result));
                    std::uint16_t buf = emit_push_const(buffers_, instructions_, val);
                    if (buf == BufferAllocator::BUFFER_UNUSED) {
                        error("E101", "Buffer pool exhausted", n.location);
                        return TypedValue::void_val();
                    }
                    source_locations_.push_back(n.location);
                    return cache_and_return(node, TypedValue::number(buf));
                } else {
                    // Array result
                    const auto& arr = std::get<std::vector<double>>(*result);
                    std::vector<std::uint16_t> result_buffers;
                    for (double v : arr) {
                        std::uint16_t buf = emit_push_const(buffers_, instructions_,
                                                             static_cast<float>(v));
                        if (buf == BufferAllocator::BUFFER_UNUSED) {
                            error("E101", "Buffer pool exhausted", n.location);
                            return TypedValue::void_val();
                        }
                        source_locations_.push_back(n.location);
                        result_buffers.push_back(buf);
                    }
                    std::uint16_t first = result_buffers.empty() ?
                        BufferAllocator::BUFFER_UNUSED : result_buffers[0];
                    std::vector<TypedValue> elements;
                    for (auto b : result_buffers) elements.push_back(TypedValue::signal(b));
                    auto tv = TypedValue::make_array(std::move(elements), first);
                    return cache_and_return(node, tv);
                }
            }
            // Fall through to normal inlining if const eval fails
        }
    }


    // Detect ..record / ..array spread arguments. Spread expansion handles
    // its own arg-to-param binding (positional + name match) inline below,
    // bypassing the legacy positional-only path.
    bool has_spread = false;
    {
        NodeIndex it = n.first_child;
        while (it != NULL_NODE) {
            const Node& a = ast_->arena[it];
            if (a.type == NodeType::Argument &&
                std::holds_alternative<Node::ArgumentData>(a.data) &&
                a.as_argument().spread_source != NULL_NODE) {
                has_spread = true;
                break;
            }
            it = ast_->arena[it].next_sibling;
        }
    }

    // Collect call arguments. Without spread, this is a flat NodeIndex list
    // for the legacy binding path. With spread, args is left empty here and
    // the spread-aware binding loop below builds param_bufs directly.
    std::vector<NodeIndex> args;
    if (!has_spread) {
        NodeIndex arg = n.first_child;
        while (arg != NULL_NODE) {
            const Node& arg_node = ast_->arena[arg];
            NodeIndex arg_value = arg;
            if (arg_node.type == NodeType::Argument) {
                arg_value = arg_node.first_child;
            }
            args.push_back(arg_value);
            arg = ast_->arena[arg].next_sibling;
        }
    }

    // Save param_literals, param_string_defaults, param_multi_buffer_sources, and param_function_refs for this scope
    auto saved_param_literals = std::move(param_literals_);
    auto saved_param_string_defaults = std::move(param_string_defaults_);
    auto saved_param_multi_buffer_sources = std::move(param_multi_buffer_sources_);
    auto saved_param_function_refs = std::move(param_function_refs_);
    param_literals_.clear();
    param_string_defaults_.clear();
    param_multi_buffer_sources_.clear();
    param_function_refs_.clear();

    // IMPORTANT: Visit arguments BEFORE pushing scope to evaluate them in caller's context
    // This allows nested function calls like double(double(x)) to work correctly.
    std::vector<std::uint16_t> param_bufs;
    // Parallel: each entry is empty unless the corresponding arg evaluated to a
    // multi-element Array. When non-empty, the param is bound via define_array
    // so arr[i] inside the body sees an Array TypedValue and emits ARRAY_PACK +
    // ARRAY_INDEX with the correct length, instead of just the first element's
    // buffer with length=1.
    std::vector<std::vector<std::uint16_t>> param_multi_bufs(func.params.size());
    // Track rest param info separately
    std::string rest_param_name;
    std::vector<std::uint16_t> rest_buffers;

    if (has_spread) {
        // Phase 3b: spread + destructure-param is deferred. Reject before
        // expansion so the user gets a clear pointer at the alternative.
        for (const auto& p : func.params) {
            if (p.is_destructure) {
                error("E105",
                      "Cannot use spread (`..arg`) to fill a destructure parameter "
                      "in '" + func.name + "'; pass a direct record value instead",
                      n.location);
                param_literals_ = std::move(saved_param_literals);
                param_string_defaults_ = std::move(saved_param_string_defaults);
                param_multi_buffer_sources_ = std::move(saved_param_multi_buffer_sources);
                param_function_refs_ = std::move(saved_param_function_refs);
                return TypedValue::error_val();
            }
        }
        // Expand ..record / ..array sources into a flat list of (positional, named) entries.
        auto expanded_opt = expand_call_arguments(node);
        if (!expanded_opt) {
            param_literals_ = std::move(saved_param_literals);
            param_string_defaults_ = std::move(saved_param_string_defaults);
            param_multi_buffer_sources_ = std::move(saved_param_multi_buffer_sources);
            param_function_refs_ = std::move(saved_param_function_refs);
            return TypedValue::error_val();
        }
        std::vector<const ExpandedArg*> positional;
        std::unordered_map<std::string, const ExpandedArg*> by_name;
        for (const auto& ea : *expanded_opt) {
            if (ea.name) {
                by_name[*ea.name] = &ea;
            } else {
                positional.push_back(&ea);
            }
        }

        // Bind each parameter from positional first, then by name, then default.
        std::set<std::string> consumed_names;
        for (std::size_t i = 0; i < func.params.size(); ++i) {
            const auto& param = func.params[i];

            if (param.is_rest) {
                // Rest + spread is unsupported in this phase — keep semantics simple.
                rest_param_name = param.name;
                for (std::size_t j = i; j < positional.size(); ++j) {
                    const ExpandedArg* ea = positional[j];
                    std::uint16_t buf;
                    if (ea->resolved) {
                        buf = ea->resolved->buffer;
                    } else {
                        buf = visit(ea->source_node).buffer;
                    }
                    rest_buffers.push_back(buf);
                }
                positional.clear();  // consumed
                std::uint16_t pbuf = rest_buffers.empty() ? BufferAllocator::BUFFER_UNUSED : rest_buffers[0];
                param_bufs.push_back(pbuf);
                break;
            }

            const ExpandedArg* arg_to_use = nullptr;
            if (i < positional.size()) {
                arg_to_use = positional[i];
            } else if (auto it = by_name.find(param.name); it != by_name.end()) {
                arg_to_use = it->second;
                consumed_names.insert(param.name);
            }

            std::uint16_t param_buf = BufferAllocator::BUFFER_UNUSED;
            if (arg_to_use) {
                if (arg_to_use->resolved) {
                    const TypedValue& tv = *arg_to_use->resolved;
                    param_buf = tv.buffer;
                    if (tv.type == ValueType::Array && tv.array &&
                        tv.array->elements.size() > 1) {
                        param_multi_bufs[i] = buffers_of(tv);
                    }
                } else {
                    // Concrete arg coming through the spread call (positional or named).
                    NodeIndex src = arg_to_use->source_node;
                    const Node& arg_node = ast_->arena[src];
                    if (arg_node.type == NodeType::StringLit ||
                        arg_node.type == NodeType::NumberLit ||
                        arg_node.type == NodeType::BoolLit) {
                        std::uint32_t param_hash = fnv1a_hash(param.name);
                        param_literals_[param_hash] = src;
                    }
                    TypedValue tv = visit(src);
                    param_buf = tv.buffer;
                    if (is_multi_buffer(src)) {
                        std::uint32_t param_hash = fnv1a_hash(param.name);
                        param_multi_buffer_sources_[param_hash] = src;
                    }
                    if (tv.type == ValueType::Array && tv.array &&
                        tv.array->elements.size() > 1) {
                        param_multi_bufs[i] = buffers_of(tv);
                    }
                }
            } else if (param.default_value.has_value()) {
                param_buf = buffers_.allocate();
                if (param_buf == BufferAllocator::BUFFER_UNUSED) {
                    error("E101", "Buffer pool exhausted", n.location);
                    param_literals_ = std::move(saved_param_literals);
                    param_string_defaults_ = std::move(saved_param_string_defaults);
                    param_multi_buffer_sources_ = std::move(saved_param_multi_buffer_sources);
                    param_function_refs_ = std::move(saved_param_function_refs);
                    return TypedValue::void_val();
                }
                cedar::Instruction push_inst{};
                push_inst.opcode = cedar::Opcode::PUSH_CONST;
                push_inst.out_buffer = param_buf;
                push_inst.inputs[0] = 0xFFFF;
                push_inst.inputs[1] = 0xFFFF;
                push_inst.inputs[2] = 0xFFFF;
                push_inst.inputs[3] = 0xFFFF;
                encode_const_value(push_inst, static_cast<float>(*param.default_value));
                emit(push_inst);
            } else if (param.default_string.has_value()) {
                param_buf = BufferAllocator::BUFFER_UNUSED;
                std::uint32_t param_hash = fnv1a_hash(param.name);
                param_string_defaults_[param_hash] = *param.default_string;
            } else if (param.default_node != NULL_NODE) {
                ConstEvaluator evaluator(*ast_, *symbols_);
                auto result = evaluator.evaluate(param.default_node);
                for (const auto& diag : evaluator.diagnostics()) {
                    diagnostics_.push_back(diag);
                }
                if (result && std::holds_alternative<double>(*result)) {
                    float val = static_cast<float>(std::get<double>(*result));
                    param_buf = emit_push_const(buffers_, instructions_, val);
                } else {
                    error("E105",
                          "Cannot evaluate default expression at compile time for parameter '" +
                          param.name + "'", n.location);
                    param_literals_ = std::move(saved_param_literals);
                    param_string_defaults_ = std::move(saved_param_string_defaults);
                    param_multi_buffer_sources_ = std::move(saved_param_multi_buffer_sources);
                    param_function_refs_ = std::move(saved_param_function_refs);
                    return TypedValue::void_val();
                }
            } else {
                error("E105", "Missing required argument for parameter '" +
                      param.name + "'", n.location);
                param_literals_ = std::move(saved_param_literals);
                param_string_defaults_ = std::move(saved_param_string_defaults);
                param_multi_buffer_sources_ = std::move(saved_param_multi_buffer_sources);
                param_function_refs_ = std::move(saved_param_function_refs);
                return TypedValue::void_val();
            }
            param_bufs.push_back(param_buf);
        }

        // Validate excess positional and unmatched named (W160).
        if (!func.has_rest_param && positional.size() > func.params.size()) {
            error("E107", "Too many arguments — function '" + func.name + "' takes " +
                  std::to_string(func.params.size()) + ", got " +
                  std::to_string(positional.size()), n.location);
            param_literals_ = std::move(saved_param_literals);
            param_string_defaults_ = std::move(saved_param_string_defaults);
            param_multi_buffer_sources_ = std::move(saved_param_multi_buffer_sources);
            param_function_refs_ = std::move(saved_param_function_refs);
            return TypedValue::error_val();
        }
        for (const auto& [name, ea] : by_name) {
            if (consumed_names.find(name) == consumed_names.end()) {
                std::string params_sig;
                for (std::size_t i = 0; i < func.params.size(); ++i) {
                    if (i > 0) params_sig += ", ";
                    params_sig += func.params[i].name;
                }
                warn("W160", "Spread field '" + name +
                     "' has no matching parameter in " + func.name +
                     "(" + params_sig + ")", ea->loc);
            }
        }
    } else
    for (std::size_t i = 0; i < func.params.size(); ++i) {
        std::uint16_t param_buf;

        if (func.params[i].is_destructure) {
            // Phase 3b: visit the caller arg so node_types_ gets populated
            // for the step-2 destructure branch. Buffer here is unused (the
            // destructure binds per-field buffers, not a record-as-a-whole).
            // Missing-arg case: emit a friendlier error that doesn't leak
            // the synthetic placeholder name.
            if (i < args.size()) {
                TypedValue arg_tv = visit(args[i]);
                param_buf = arg_tv.buffer;
            } else {
                error("E105",
                      "Missing required record argument for destructure parameter "
                      "in '" + func.name + "'", n.location);
                param_literals_ = std::move(saved_param_literals);
                param_string_defaults_ = std::move(saved_param_string_defaults);
                return TypedValue::void_val();
            }
            param_bufs.push_back(param_buf);
            continue;
        }

        if (func.params[i].is_rest) {
            // Rest parameter: collect all remaining arguments
            rest_param_name = func.params[i].name;
            for (std::size_t j = i; j < args.size(); ++j) {
                std::uint16_t buf = visit(args[j]).buffer;
                rest_buffers.push_back(buf);
            }
            param_buf = rest_buffers.empty() ? BufferAllocator::BUFFER_UNUSED : rest_buffers[0];
            param_bufs.push_back(param_buf);
            break;  // Rest must be last param
        } else if (i < args.size()) {
            // Check for underscore placeholder: fill with default value
            const Node& arg_inner = ast_->arena[args[i]];
            bool is_placeholder = (arg_inner.type == NodeType::Identifier &&
                std::holds_alternative<Node::IdentifierData>(arg_inner.data) &&
                arg_inner.as_identifier() == "_");

            if (is_placeholder) {
                // Use parameter default (same logic as trailing omission below)
                if (func.params[i].default_value.has_value()) {
                    param_buf = buffers_.allocate();
                    if (param_buf == BufferAllocator::BUFFER_UNUSED) {
                        error("E101", "Buffer pool exhausted", n.location);
                        param_literals_ = std::move(saved_param_literals);
                        return TypedValue::void_val();
                    }
                    cedar::Instruction push_inst{};
                    push_inst.opcode = cedar::Opcode::PUSH_CONST;
                    push_inst.out_buffer = param_buf;
                    push_inst.inputs[0] = 0xFFFF;
                    push_inst.inputs[1] = 0xFFFF;
                    push_inst.inputs[2] = 0xFFFF;
                    push_inst.inputs[3] = 0xFFFF;
                    float default_val = static_cast<float>(*func.params[i].default_value);
                    encode_const_value(push_inst, default_val);
                    emit(push_inst);
                } else if (func.params[i].default_node != NULL_NODE &&
                           !func.params[i].default_string.has_value()) {
                    ConstEvaluator evaluator(*ast_, *symbols_);
                    auto result = evaluator.evaluate(func.params[i].default_node);
                    for (const auto& diag : evaluator.diagnostics()) {
                        diagnostics_.push_back(diag);
                    }
                    if (result && std::holds_alternative<double>(*result)) {
                        float val = static_cast<float>(std::get<double>(*result));
                        param_buf = emit_push_const(buffers_, instructions_, val);
                        if (param_buf == BufferAllocator::BUFFER_UNUSED) {
                            error("E101", "Buffer pool exhausted", n.location);
                            param_literals_ = std::move(saved_param_literals);
                            param_string_defaults_ = std::move(saved_param_string_defaults);
                            return TypedValue::void_val();
                        }
                    } else {
                        error("E105", "Cannot evaluate default expression at compile time for parameter '" +
                              func.params[i].name + "'", n.location);
                        param_literals_ = std::move(saved_param_literals);
                        param_string_defaults_ = std::move(saved_param_string_defaults);
                        return TypedValue::void_val();
                    }
                } else if (func.params[i].default_string.has_value()) {
                    param_buf = BufferAllocator::BUFFER_UNUSED;
                    std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
                    param_string_defaults_[param_hash] = *func.params[i].default_string;
                } else {
                    error("E106", "Cannot skip required parameter '" +
                          func.params[i].name + "' — no default value", n.location);
                    param_literals_ = std::move(saved_param_literals);
                    param_string_defaults_ = std::move(saved_param_string_defaults);
                    return TypedValue::void_val();
                }
            } else {
                // Check if the argument is a closure or function reference
                auto func_ref = resolve_function_arg(args[i]);
                if (func_ref) {
                    // Store as function ref — don't visit (would eagerly compile with unbound params)
                    std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
                    param_function_refs_[param_hash] = *func_ref;
                    param_buf = BufferAllocator::BUFFER_UNUSED;  // placeholder — never used as audio
                } else {
                    // Check if the argument is a literal - record for match resolution
                    const Node& arg_node = ast_->arena[args[i]];
                    if (arg_node.type == NodeType::StringLit ||
                        arg_node.type == NodeType::NumberLit ||
                        arg_node.type == NodeType::BoolLit) {
                        std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
                        param_literals_[param_hash] = args[i];
                    }

                    // Visit argument in caller's scope
                    TypedValue arg_tv = visit(args[i]);
                    param_buf = arg_tv.buffer;

                    // PRD prd-patterns-as-scalar-values §5.3 / §9.5: user-fn
                    // params get default Signal coerce. Polyphonic non-sample
                    // patterns would silently pass voice-0 — reject E160 so
                    // the user picks a voice or wraps with poly().
                    if (arg_tv.type == ValueType::Pattern && arg_tv.pattern &&
                        arg_tv.pattern->max_voices > 1 &&
                        !arg_tv.pattern->is_sample_pattern) {
                        error("E160",
                              "user function parameter '" + func.params[i].name +
                              "' cannot accept a polyphonic pattern as scalar; "
                              "use poly() to consume it, or pick a voice/field "
                              "explicitly (e.g. p.freq)",
                              ast_->arena[args[i]].location);
                    }

                    // Track multi-buffer arguments for polyphonic propagation
                    if (is_multi_buffer(args[i])) {
                        std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
                        param_multi_buffer_sources_[param_hash] = args[i];
                    }
                    // Capture array info so the param can be bound as an Array
                    // symbol (lets arr[i] inside the body emit ARRAY_PACK +
                    // ARRAY_INDEX with the correct length).
                    if (arg_tv.type == ValueType::Array && arg_tv.array &&
                        arg_tv.array->elements.size() > 1) {
                        param_multi_bufs[i] = buffers_of(arg_tv);
                    }
                }
            }
        } else if (func.params[i].default_value.has_value()) {
            // Use numeric default value
            param_buf = buffers_.allocate();
            if (param_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                param_literals_ = std::move(saved_param_literals);
                return TypedValue::void_val();
            }

            cedar::Instruction push_inst{};
            push_inst.opcode = cedar::Opcode::PUSH_CONST;
            push_inst.out_buffer = param_buf;
            push_inst.inputs[0] = 0xFFFF;
            push_inst.inputs[1] = 0xFFFF;
            push_inst.inputs[2] = 0xFFFF;
            push_inst.inputs[3] = 0xFFFF;

            float default_val = static_cast<float>(*func.params[i].default_value);
            encode_const_value(push_inst, default_val);
            emit(push_inst);
        } else if (func.params[i].default_node != NULL_NODE &&
                   !func.params[i].default_string.has_value()) {
            // Expression default: evaluate at compile time via ConstEvaluator
            ConstEvaluator evaluator(*ast_, *symbols_);
            auto result = evaluator.evaluate(func.params[i].default_node);

            for (const auto& diag : evaluator.diagnostics()) {
                diagnostics_.push_back(diag);
            }

            if (result) {
                if (std::holds_alternative<double>(*result)) {
                    float val = static_cast<float>(std::get<double>(*result));
                    param_buf = emit_push_const(buffers_, instructions_, val);
                    if (param_buf == BufferAllocator::BUFFER_UNUSED) {
                        error("E101", "Buffer pool exhausted", n.location);
                        param_literals_ = std::move(saved_param_literals);
                        param_string_defaults_ = std::move(saved_param_string_defaults);
                        return TypedValue::void_val();
                    }
                } else {
                    // Array result — not supported as default
                    error("E105", "Array expression defaults not yet supported for parameter '" +
                          func.params[i].name + "'", n.location);
                    param_literals_ = std::move(saved_param_literals);
                    param_string_defaults_ = std::move(saved_param_string_defaults);
                    return TypedValue::void_val();
                }
            } else {
                error("E105", "Cannot evaluate default expression at compile time for parameter '" +
                      func.params[i].name + "'", n.location);
                param_literals_ = std::move(saved_param_literals);
                param_string_defaults_ = std::move(saved_param_string_defaults);
                return TypedValue::void_val();
            }
        } else if (func.params[i].default_string.has_value()) {
            // String default: doesn't produce audio — used for compile-time match dispatch
            param_buf = BufferAllocator::BUFFER_UNUSED;
            std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
            param_string_defaults_[param_hash] = *func.params[i].default_string;
        } else {
            // Missing required argument - should have been caught by analyzer
            error("E105", "Missing required argument for parameter '" +
                  func.params[i].name + "'", n.location);
            param_literals_ = std::move(saved_param_literals);
            param_string_defaults_ = std::move(saved_param_string_defaults);
            return TypedValue::void_val();
        }

        param_bufs.push_back(param_buf);
    }

    // NOW push scope for function parameters and bind them
    symbols_->push_scope();
    for (std::size_t i = 0; i < func.params.size(); ++i) {
        // Phase 3b: destructure param `fn f({x, y [= default]})`. The caller
        // arg at position i must resolve to a Record TypedValue. Each declared
        // field becomes a local variable in this fresh scope; missing fields
        // fall back to declared defaults (or fire E187).
        if (func.params[i].is_destructure) {
            const TypedValue* arg_tv = nullptr;
            if (i < args.size()) {
                auto type_it = node_types_.find(args[i]);
                if (type_it != node_types_.end()) {
                    arg_tv = &type_it->second;
                }
            }
            if (arg_tv == nullptr) {
                error("E140",
                      "Cannot destructure: missing or unresolved argument for "
                      "function '" + func.name + "'", n.location);
                continue;
            }
            // bind_destructure_fields handles the Record/Pattern type check
            // and emits E140/E187 with the right messages.
            bind_destructure_fields(*arg_tv,
                                    func.params[i].destructure_fields,
                                    n.location,
                                    "E187");
            continue;
        }

        if (func.params[i].is_rest) {
            // Bind rest param as synthetic Array
            ArrayInfo arr;
            arr.source_node = NULL_NODE;
            arr.buffer_indices = rest_buffers;
            arr.element_count = rest_buffers.size();
            symbols_->define_array(func.params[i].name, arr);
        } else {
            std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
            auto fref_it = param_function_refs_.find(param_hash);
            if (fref_it != param_function_refs_.end()) {
                symbols_->define_function_value(func.params[i].name, fref_it->second);
            } else if (i < args.size()) {
                // Check if the argument produced a record
                auto type_it = node_types_.find(args[i]);
                if (type_it != node_types_.end() && type_it->second.type == ValueType::Record) {
                    auto record_type = std::make_shared<RecordTypeInfo>();
                    record_type->source_node = args[i];
                    if (type_it->second.record) {
                        for (const auto& [name, field_tv] : type_it->second.record->fields) {
                            RecordFieldInfo field_info;
                            field_info.name = name;
                            field_info.buffer_index = field_tv.buffer;
                            field_info.field_kind = SymbolKind::Variable;
                            record_type->fields.push_back(std::move(field_info));
                        }
                    }
                    symbols_->define_record(func.params[i].name, record_type);
                } else if (!param_multi_bufs[i].empty()) {
                    ArrayInfo arr;
                    arr.source_node = NULL_NODE;
                    arr.buffer_indices = param_multi_bufs[i];
                    arr.element_count = param_multi_bufs[i].size();
                    symbols_->define_array(func.params[i].name, arr);
                } else {
                    symbols_->define_variable(func.params[i].name, param_bufs[i]);
                }
            } else {
                symbols_->define_variable(func.params[i].name, param_bufs[i]);
            }
        }
    }

    // Check if function was explicitly defined to return a closure
    // (e.g. fn make_filter(cut) -> (sig) -> lp(sig, cut))
    // Only triggers for source-level closures, not pipe-rewritten ones
    if (func.returns_closure && func.body_node != NULL_NODE &&
        ast_->arena[func.body_node].type == NodeType::Closure) {
        // Build FunctionRef from the closure body
        auto ref = resolve_function_arg(func.body_node);
        if (ref) {
            // Add captures for bound function parameters
            for (std::size_t i = 0; i < func.params.size(); ++i) {
                std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
                auto fref_it = param_function_refs_.find(param_hash);
                if (fref_it != param_function_refs_.end()) {
                    // Param is itself a function ref — add its captures transitively
                    // (the closure body can reference the outer fn's function params)
                    // For now, skip — function params are resolved via param_function_refs_
                } else if (param_bufs[i] != BufferAllocator::BUFFER_UNUSED) {
                    ref->captures.push_back({func.params[i].name, param_bufs[i]});
                }
                // Also propagate string defaults as captures won't help for strings,
                // but the param_string_defaults_ will be set when the closure is later called
            }
            pending_function_ref_ = *ref;
        }

        // Pop scope and restore state
        symbols_->pop_scope();
        param_literals_ = std::move(saved_param_literals);
        param_string_defaults_ = std::move(saved_param_string_defaults);
        param_multi_buffer_sources_ = std::move(saved_param_multi_buffer_sources);
        param_function_refs_ = std::move(saved_param_function_refs);

        return cache_and_return(node, TypedValue::function_val());
    }

    // Save node_types_ state before visiting body.
    // This is necessary because function bodies are shared AST nodes that may be
    // visited multiple times with different parameter bindings.
    auto saved_node_types = std::move(node_types_);
    node_types_.clear();

    // Visit function body (inline expansion)
    TypedValue result_tv = TypedValue::void_val();
    if (func.body_node != NULL_NODE) {
        result_tv = visit(func.body_node);
    }

    // Restore node_types_ (keep new entries but restore old ones)
    for (auto& [k, v] : saved_node_types) {
        if (node_types_.find(k) == node_types_.end()) {
            node_types_[k] = v;
        }
    }

    // Pop scope and restore param_literals, param_string_defaults, param_multi_buffer_sources, and param_function_refs
    symbols_->pop_scope();
    param_literals_ = std::move(saved_param_literals);
    param_string_defaults_ = std::move(saved_param_string_defaults);
    param_multi_buffer_sources_ = std::move(saved_param_multi_buffer_sources);
    param_function_refs_ = std::move(saved_param_function_refs);

    return cache_and_return(node, result_tv);
}

// FunctionValue (lambda variable) call handler - inlines closure bodies at call sites
TypedValue CodeGenerator::handle_function_value_call(
    NodeIndex node, const Node& n, const FunctionRef& func) {

    // Detect ..record / ..array spread in the call.
    bool has_spread = false;
    {
        NodeIndex it = n.first_child;
        while (it != NULL_NODE) {
            const Node& a = ast_->arena[it];
            if (a.type == NodeType::Argument &&
                std::holds_alternative<Node::ArgumentData>(a.data) &&
                a.as_argument().spread_source != NULL_NODE) {
                has_spread = true;
                break;
            }
            it = ast_->arena[it].next_sibling;
        }
    }

    // Collect call arguments (legacy positional-only path; spread takes a separate branch).
    std::vector<NodeIndex> args;
    if (!has_spread) {
        NodeIndex arg = n.first_child;
        while (arg != NULL_NODE) {
            const Node& arg_node = ast_->arena[arg];
            NodeIndex arg_value = arg;
            if (arg_node.type == NodeType::Argument) {
                arg_value = arg_node.first_child;
            }
            args.push_back(arg_value);
            arg = ast_->arena[arg].next_sibling;
        }
    }

    // Save param_literals and param_string_defaults for this scope
    auto saved_param_literals = std::move(param_literals_);
    auto saved_param_string_defaults = std::move(param_string_defaults_);
    param_literals_.clear();
    param_string_defaults_.clear();

    // Visit arguments BEFORE pushing scope to evaluate them in caller's context
    std::vector<std::uint16_t> param_bufs;
    // Parallel: each entry is empty unless the corresponding arg evaluated to a
    // multi-element Array. When non-empty, the param is bound via define_array
    // so arr[i] inside the lambda body sees an Array TypedValue (and emits
    // ARRAY_PACK + ARRAY_INDEX with the correct length) instead of just the
    // first element's buffer with length=1.
    std::vector<std::vector<std::uint16_t>> param_multi_bufs(func.params.size());

    if (has_spread) {
        auto expanded_opt = expand_call_arguments(node);
        if (!expanded_opt) {
            param_literals_ = std::move(saved_param_literals);
            param_string_defaults_ = std::move(saved_param_string_defaults);
            return TypedValue::error_val();
        }
        std::vector<const ExpandedArg*> positional;
        std::unordered_map<std::string, const ExpandedArg*> by_name;
        for (const auto& ea : *expanded_opt) {
            if (ea.name) by_name[*ea.name] = &ea;
            else positional.push_back(&ea);
        }
        std::set<std::string> consumed_names;
        for (std::size_t i = 0; i < func.params.size(); ++i) {
            const auto& param = func.params[i];
            const ExpandedArg* arg_to_use = nullptr;
            if (i < positional.size()) {
                arg_to_use = positional[i];
            } else if (auto it = by_name.find(param.name); it != by_name.end()) {
                arg_to_use = it->second;
                consumed_names.insert(param.name);
            }
            std::uint16_t param_buf = BufferAllocator::BUFFER_UNUSED;
            if (arg_to_use) {
                if (arg_to_use->resolved) {
                    const TypedValue& tv = *arg_to_use->resolved;
                    param_buf = tv.buffer;
                    if (tv.type == ValueType::Array && tv.array &&
                        tv.array->elements.size() > 1) {
                        param_multi_bufs[i] = buffers_of(tv);
                    }
                } else {
                    NodeIndex src = arg_to_use->source_node;
                    const Node& arg_node = ast_->arena[src];
                    if (arg_node.type == NodeType::StringLit ||
                        arg_node.type == NodeType::NumberLit ||
                        arg_node.type == NodeType::BoolLit) {
                        std::uint32_t param_hash = fnv1a_hash(param.name);
                        param_literals_[param_hash] = src;
                    }
                    TypedValue tv = visit(src);
                    param_buf = tv.buffer;
                    if (tv.type == ValueType::Array && tv.array &&
                        tv.array->elements.size() > 1) {
                        param_multi_bufs[i] = buffers_of(tv);
                    }
                }
            } else if (param.default_value.has_value()) {
                param_buf = buffers_.allocate();
                if (param_buf == BufferAllocator::BUFFER_UNUSED) {
                    error("E101", "Buffer pool exhausted", n.location);
                    param_literals_ = std::move(saved_param_literals);
                    param_string_defaults_ = std::move(saved_param_string_defaults);
                    return TypedValue::void_val();
                }
                cedar::Instruction push_inst{};
                push_inst.opcode = cedar::Opcode::PUSH_CONST;
                push_inst.out_buffer = param_buf;
                push_inst.inputs[0] = 0xFFFF;
                push_inst.inputs[1] = 0xFFFF;
                push_inst.inputs[2] = 0xFFFF;
                push_inst.inputs[3] = 0xFFFF;
                encode_const_value(push_inst, static_cast<float>(*param.default_value));
                emit(push_inst);
            } else if (param.default_string.has_value()) {
                param_buf = BufferAllocator::BUFFER_UNUSED;
                std::uint32_t param_hash = fnv1a_hash(param.name);
                param_string_defaults_[param_hash] = *param.default_string;
            } else if (param.default_node != NULL_NODE) {
                ConstEvaluator evaluator(*ast_, *symbols_);
                auto result = evaluator.evaluate(param.default_node);
                for (const auto& diag : evaluator.diagnostics()) {
                    diagnostics_.push_back(diag);
                }
                if (result && std::holds_alternative<double>(*result)) {
                    float val = static_cast<float>(std::get<double>(*result));
                    param_buf = emit_push_const(buffers_, instructions_, val);
                } else {
                    error("E105",
                          "Cannot evaluate default expression at compile time for parameter '" +
                          param.name + "'", n.location);
                    param_literals_ = std::move(saved_param_literals);
                    param_string_defaults_ = std::move(saved_param_string_defaults);
                    return TypedValue::void_val();
                }
            } else {
                error("E105", "Missing required argument for parameter '" +
                      param.name + "'", n.location);
                param_literals_ = std::move(saved_param_literals);
                param_string_defaults_ = std::move(saved_param_string_defaults);
                return TypedValue::void_val();
            }
            param_bufs.push_back(param_buf);
        }

        if (positional.size() > func.params.size()) {
            error("E107", "Too many arguments — closure takes " +
                  std::to_string(func.params.size()) + ", got " +
                  std::to_string(positional.size()), n.location);
            param_literals_ = std::move(saved_param_literals);
            param_string_defaults_ = std::move(saved_param_string_defaults);
            return TypedValue::error_val();
        }
        for (const auto& [name, ea] : by_name) {
            if (consumed_names.find(name) == consumed_names.end()) {
                std::string params_sig;
                for (std::size_t i = 0; i < func.params.size(); ++i) {
                    if (i > 0) params_sig += ", ";
                    params_sig += func.params[i].name;
                }
                warn("W160", "Spread field '" + name +
                     "' has no matching parameter in closure(" + params_sig + ")",
                     ea->loc);
            }
        }
    } else
    for (std::size_t i = 0; i < func.params.size(); ++i) {
        std::uint16_t param_buf;

        if (i < args.size()) {
            // Check if the argument is a literal - record for match resolution
            const Node& arg_node = ast_->arena[args[i]];
            if (arg_node.type == NodeType::StringLit ||
                arg_node.type == NodeType::NumberLit ||
                arg_node.type == NodeType::BoolLit) {
                std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
                param_literals_[param_hash] = args[i];
            }

            // Visit argument in caller's scope
            TypedValue arg_tv = visit(args[i]);
            param_buf = arg_tv.buffer;
            if (arg_tv.type == ValueType::Array && arg_tv.array &&
                arg_tv.array->elements.size() > 1) {
                param_multi_bufs[i] = buffers_of(arg_tv);
            }
        } else if (func.params[i].default_value.has_value()) {
            // Use numeric default value
            param_buf = buffers_.allocate();
            if (param_buf == BufferAllocator::BUFFER_UNUSED) {
                error("E101", "Buffer pool exhausted", n.location);
                param_literals_ = std::move(saved_param_literals);
                param_string_defaults_ = std::move(saved_param_string_defaults);
                return TypedValue::void_val();
            }

            cedar::Instruction push_inst{};
            push_inst.opcode = cedar::Opcode::PUSH_CONST;
            push_inst.out_buffer = param_buf;
            push_inst.inputs[0] = 0xFFFF;
            push_inst.inputs[1] = 0xFFFF;
            push_inst.inputs[2] = 0xFFFF;
            push_inst.inputs[3] = 0xFFFF;

            float default_val = static_cast<float>(*func.params[i].default_value);
            encode_const_value(push_inst, default_val);
            emit(push_inst);
        } else if (func.params[i].default_node != NULL_NODE &&
                   !func.params[i].default_string.has_value()) {
            // Expression default: evaluate at compile time via ConstEvaluator
            ConstEvaluator evaluator(*ast_, *symbols_);
            auto result = evaluator.evaluate(func.params[i].default_node);

            for (const auto& diag : evaluator.diagnostics()) {
                diagnostics_.push_back(diag);
            }

            if (result) {
                if (std::holds_alternative<double>(*result)) {
                    float val = static_cast<float>(std::get<double>(*result));
                    param_buf = emit_push_const(buffers_, instructions_, val);
                    if (param_buf == BufferAllocator::BUFFER_UNUSED) {
                        error("E101", "Buffer pool exhausted", n.location);
                        param_literals_ = std::move(saved_param_literals);
                        param_string_defaults_ = std::move(saved_param_string_defaults);
                        return TypedValue::void_val();
                    }
                } else {
                    error("E105", "Array expression defaults not yet supported for parameter '" +
                          func.params[i].name + "'", n.location);
                    param_literals_ = std::move(saved_param_literals);
                    param_string_defaults_ = std::move(saved_param_string_defaults);
                    return TypedValue::void_val();
                }
            } else {
                error("E105", "Cannot evaluate default expression at compile time for parameter '" +
                      func.params[i].name + "'", n.location);
                param_literals_ = std::move(saved_param_literals);
                param_string_defaults_ = std::move(saved_param_string_defaults);
                return TypedValue::void_val();
            }
        } else if (func.params[i].default_string.has_value()) {
            // String default: doesn't produce audio — used for compile-time match dispatch
            param_buf = BufferAllocator::BUFFER_UNUSED;
            std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
            param_string_defaults_[param_hash] = *func.params[i].default_string;
        } else {
            // Missing required argument - should have been caught by analyzer
            error("E105", "Missing required argument for parameter '" +
                  func.params[i].name + "'", n.location);
            param_literals_ = std::move(saved_param_literals);
            param_string_defaults_ = std::move(saved_param_string_defaults);
            return TypedValue::void_val();
        }

        param_bufs.push_back(param_buf);
    }

    // Push scope for captures and parameters
    symbols_->push_scope();
    for (const auto& capture : func.captures) {
        symbols_->define_variable(capture.name, capture.buffer_index);
    }
    for (std::size_t i = 0; i < func.params.size(); ++i) {
        if (!param_multi_bufs[i].empty()) {
            ArrayInfo arr;
            arr.source_node = NULL_NODE;
            arr.buffer_indices = param_multi_bufs[i];
            arr.element_count = param_multi_bufs[i].size();
            symbols_->define_array(func.params[i].name, arr);
        } else {
            symbols_->define_variable(func.params[i].name, param_bufs[i]);
        }
    }

    // Push semantic path for unique state IDs
    const std::string& callee_name = n.as_identifier();
    std::uint32_t count = call_counters_[callee_name]++;
    push_path(callee_name + "#" + std::to_string(count));

    // Save node_types_ state before visiting body
    auto saved_node_types = std::move(node_types_);
    node_types_.clear();

    TypedValue result_tv = TypedValue::void_val();

    // Handle composed functions: apply each function in the chain sequentially
    if (!func.compose_chain.empty()) {
        // First function gets the call argument(s)
        std::uint16_t result = BufferAllocator::BUFFER_UNUSED;
        if (!param_bufs.empty()) {
            result = param_bufs[0];
        }
        for (const auto& chain_ref : func.compose_chain) {
            result = apply_function_ref(chain_ref, result, n.location);
        }
        result_tv = TypedValue::signal(result);
    } else {
        // Normal function value call
        // Find the body node
        NodeIndex body = NULL_NODE;
        if (func.is_user_function) {
            body = func.closure_node;
        } else {
            const Node& closure_node = ast_->arena[func.closure_node];
            NodeIndex child = closure_node.first_child;
            while (child != NULL_NODE) {
                const Node& child_node = ast_->arena[child];
                if (child_node.type == NodeType::Identifier) {
                    // parameter — skip
                } else {
                    body = child;
                    break;
                }
                child = ast_->arena[child].next_sibling;
            }
        }

        // Visit closure body (inline expansion)
        if (body != NULL_NODE) {
            result_tv = visit(body);
        }
    }

    // Restore node_types_
    for (auto& [k, v] : saved_node_types) {
        if (node_types_.find(k) == node_types_.end()) {
            node_types_[k] = v;
        }
    }

    // Pop semantic path, scope, and restore param_literals and param_string_defaults
    pop_path();
    symbols_->pop_scope();
    param_literals_ = std::move(saved_param_literals);
    param_string_defaults_ = std::move(saved_param_string_defaults);

    return cache_and_return(node, result_tv);
}

// Handle compose(f, g, ...) - create a composed function reference
TypedValue CodeGenerator::handle_compose_call(NodeIndex node, const Node& n) {
    // Collect function arguments
    std::vector<FunctionRef> refs;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        const Node& arg_node = ast_->arena[arg];
        NodeIndex arg_value = arg;
        if (arg_node.type == NodeType::Argument) {
            arg_value = arg_node.first_child;
        }
        auto ref = resolve_function_arg(arg_value);
        if (!ref) {
            error("E140", "compose() arguments must be functions or closures", arg_node.location);
            return TypedValue::void_val();
        }
        refs.push_back(*ref);
        arg = ast_->arena[arg].next_sibling;
    }

    if (refs.size() < 2) {
        error("E141", "compose() requires at least 2 function arguments", n.location);
        return TypedValue::void_val();
    }

    // Build composed FunctionRef: first function's params, chain of all refs
    FunctionRef composed{};
    composed.params = refs[0].params;  // Same params as first function
    composed.closure_node = refs[0].closure_node;
    composed.is_user_function = false;
    composed.captures = refs[0].captures;
    composed.compose_chain = refs;  // Store entire chain

    pending_function_ref_ = composed;
    return cache_and_return(node, TypedValue::function_val());
}

// Handle Closure nodes - allocate buffers for parameters and generate body
TypedValue CodeGenerator::handle_closure(NodeIndex node, const Node& n) {
    // For simple closures: allocate buffers for parameters, then generate body
    // Find parameters and body
    // Parameters may be stored as IdentifierData or ClosureParamData
    std::vector<std::string> param_names;
    NodeIndex child = n.first_child;
    NodeIndex body = NULL_NODE;

    while (child != NULL_NODE) {
        const Node& child_node = ast_->arena[child];
        if (child_node.type == NodeType::Identifier) {
            // Check if it's IdentifierData or ClosureParamData
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
        error("E112", "Closure has no body", n.location);
        return TypedValue::void_val();
    }

    // Allocate input buffers for parameters and bind them
    for (const auto& param : param_names) {
        std::uint16_t param_buf = buffers_.allocate();
        if (param_buf == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }
        // Update symbol table with actual buffer index
        symbols_->define_variable(param, param_buf);
    }

    // Generate code for body
    auto body_tv = visit(body);
    return cache_and_return(node, body_tv);
}

// Check if a match expression can be resolved at compile time
bool CodeGenerator::is_compile_time_match(NodeIndex node, const Node& n) const {
    // Get match expression data
    if (!std::holds_alternative<Node::MatchExprData>(n.data)) {
        return false;
    }
    const auto& match_data = n.as_match_expr();

    // For guard-only form without scrutinee, we need runtime evaluation
    // unless all guards are compile-time constants
    if (!match_data.has_scrutinee) {
        // Guard-only: check if all guards are compile-time evaluable
        NodeIndex arm = n.first_child;
        while (arm != NULL_NODE) {
            const Node& arm_node = ast_->arena[arm];
            if (arm_node.type == NodeType::MatchArm) {
                const auto& arm_data = arm_node.as_match_arm();
                if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                    const Node& guard_node = ast_->arena[arm_data.guard_node];
                    // Only simple literals are compile-time evaluable
                    if (guard_node.type != NodeType::BoolLit &&
                        guard_node.type != NodeType::NumberLit) {
                        return false;  // Non-const guard -> runtime
                    }
                }
            }
            arm = ast_->arena[arm].next_sibling;
        }
        return true;
    }

    // Scrutinee form: check if scrutinee resolves to a literal
    NodeIndex scrutinee = n.first_child;
    if (scrutinee == NULL_NODE) return false;

    const Node* scrutinee_ptr = &ast_->arena[scrutinee];

    // If scrutinee is an Identifier, check if it maps to a literal argument or string default
    if (scrutinee_ptr->type == NodeType::Identifier) {
        std::string param_name;
        if (std::holds_alternative<Node::ClosureParamData>(scrutinee_ptr->data)) {
            param_name = scrutinee_ptr->as_closure_param().name;
        } else if (std::holds_alternative<Node::IdentifierData>(scrutinee_ptr->data)) {
            param_name = scrutinee_ptr->as_identifier();
        }

        if (!param_name.empty()) {
            std::uint32_t param_hash = fnv1a_hash(param_name);
            auto it = param_literals_.find(param_hash);
            if (it != param_literals_.end()) {
                scrutinee_ptr = &ast_->arena[it->second];
            } else if (param_string_defaults_.count(param_hash)) {
                // String default found — compile-time match is possible
                // (handled in handle_compile_time_match via param_string_defaults_)
                scrutinee_ptr = nullptr;  // Signal: use string default path below
            } else {
                return false;  // Variable without known literal -> runtime
            }
        }
    }

    // Check if scrutinee is a literal (or string default)
    if (scrutinee_ptr != nullptr &&
        scrutinee_ptr->type != NodeType::StringLit &&
        scrutinee_ptr->type != NodeType::NumberLit &&
        scrutinee_ptr->type != NodeType::BoolLit) {
        return false;  // Non-literal scrutinee -> runtime
    }

    // Check all guards for const-evaluability
    NodeIndex arm = ast_->arena[scrutinee].next_sibling;
    while (arm != NULL_NODE) {
        const Node& arm_node = ast_->arena[arm];
        if (arm_node.type == NodeType::MatchArm) {
            const auto& arm_data = arm_node.as_match_arm();
            if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                const Node& guard_node = ast_->arena[arm_data.guard_node];
                if (guard_node.type != NodeType::BoolLit &&
                    guard_node.type != NodeType::NumberLit) {
                    return false;  // Non-const guard -> runtime
                }
            }
        }
        arm = ast_->arena[arm].next_sibling;
    }

    return true;
}

// Handle compile-time match - evaluate patterns and guards, emit only winning branch
TypedValue CodeGenerator::handle_compile_time_match(NodeIndex node, const Node& n) {
    const auto& match_data = n.as_match_expr();

    NodeIndex first_arm = n.first_child;
    const Node* scrutinee_ptr = nullptr;
    std::string scrutinee_key;

    if (match_data.has_scrutinee) {
        NodeIndex scrutinee = n.first_child;
        first_arm = ast_->arena[scrutinee].next_sibling;
        scrutinee_ptr = &ast_->arena[scrutinee];

        // If scrutinee is an Identifier, check if it maps to a literal argument or string default
        if (scrutinee_ptr->type == NodeType::Identifier) {
            std::string param_name;
            if (std::holds_alternative<Node::ClosureParamData>(scrutinee_ptr->data)) {
                param_name = scrutinee_ptr->as_closure_param().name;
            } else if (std::holds_alternative<Node::IdentifierData>(scrutinee_ptr->data)) {
                param_name = scrutinee_ptr->as_identifier();
            }

            if (!param_name.empty()) {
                std::uint32_t param_hash = fnv1a_hash(param_name);
                auto it = param_literals_.find(param_hash);
                if (it != param_literals_.end()) {
                    scrutinee_ptr = &ast_->arena[it->second];
                } else {
                    auto str_it = param_string_defaults_.find(param_hash);
                    if (str_it != param_string_defaults_.end()) {
                        // String default — set key directly, no AST node needed
                        scrutinee_key = "s:" + str_it->second;
                        scrutinee_ptr = nullptr;  // Skip node-based key extraction below
                    }
                }
            }
        }

        // Get scrutinee value for matching (skip if already set from string default)
        if (scrutinee_ptr != nullptr) {
            if (scrutinee_ptr->type == NodeType::StringLit) {
                scrutinee_key = "s:" + scrutinee_ptr->as_string();
            } else if (scrutinee_ptr->type == NodeType::NumberLit) {
                scrutinee_key = "n:" + std::to_string(scrutinee_ptr->as_number());
            } else if (scrutinee_ptr->type == NodeType::BoolLit) {
                scrutinee_key = "b:" + std::to_string(scrutinee_ptr->as_bool());
            }
        }
    }

    // Find matching arm
    NodeIndex arm = first_arm;
    NodeIndex default_body = NULL_NODE;

    while (arm != NULL_NODE) {
        const Node& arm_node = ast_->arena[arm];
        if (arm_node.type == NodeType::MatchArm) {
            const auto& arm_data = arm_node.as_match_arm();

            // Get pattern (first child) and body (second child)
            NodeIndex pattern = arm_node.first_child;
            NodeIndex body = (pattern != NULL_NODE) ?
                            ast_->arena[pattern].next_sibling : NULL_NODE;

            if (arm_data.is_wildcard) {
                default_body = body;
            } else if (arm_data.is_destructure && match_data.has_scrutinee) {
                // Destructuring pattern: always matches, bind fields from scrutinee
                NodeIndex scrutinee_node = n.first_child;
                auto scrutinee_tv = visit(scrutinee_node);  // ensure node_types_ populated

                // Check guard if present
                bool guard_passes = true;
                if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                    const Node& guard_node = ast_->arena[arm_data.guard_node];
                    if (guard_node.type == NodeType::BoolLit) {
                        guard_passes = guard_node.as_bool();
                    } else if (guard_node.type == NodeType::NumberLit) {
                        guard_passes = guard_node.as_number() != 0.0;
                    }
                }

                if (guard_passes && body != NULL_NODE) {
                    symbols_->push_scope();
                    // Match-arm destructure has no default expressions —
                    // wrap names into DestructureField{name, NULL_NODE}.
                    std::vector<DestructureField> arm_fields;
                    arm_fields.reserve(arm_data.destructure_fields.size());
                    for (const auto& fname : arm_data.destructure_fields) {
                        arm_fields.push_back(DestructureField{fname, NULL_NODE});
                    }
                    bind_destructure_fields(scrutinee_tv, arm_fields, arm_node.location);
                    auto result_tv = visit(body);
                    symbols_->pop_scope();
                    return cache_and_return(node, result_tv);
                }
            } else if (match_data.has_scrutinee) {
                // Range pattern: check low <= scrutinee < high
                if (arm_data.is_range) {
                    // Extract numeric scrutinee value
                    double scrutinee_val = 0.0;
                    bool is_numeric = false;
                    if (scrutinee_ptr != nullptr && scrutinee_ptr->type == NodeType::NumberLit) {
                        scrutinee_val = scrutinee_ptr->as_number();
                        is_numeric = true;
                    }

                    if (is_numeric && scrutinee_val >= arm_data.range_low && scrutinee_val < arm_data.range_high) {
                        // Range matches - check guard if present
                        bool guard_passes = true;
                        if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                            const Node& guard_node = ast_->arena[arm_data.guard_node];
                            if (guard_node.type == NodeType::BoolLit) {
                                guard_passes = guard_node.as_bool();
                            } else if (guard_node.type == NodeType::NumberLit) {
                                guard_passes = guard_node.as_number() != 0.0;
                            }
                        }

                        if (guard_passes && body != NULL_NODE) {
                            auto result_tv = visit(body);
                            return cache_and_return(node, result_tv);
                        }
                    }
                } else if (pattern != NULL_NODE) {
                // Scrutinee form: check pattern match
                    const Node& pattern_node = ast_->arena[pattern];
                    std::string pattern_key;

                    if (pattern_node.type == NodeType::StringLit) {
                        pattern_key = "s:" + pattern_node.as_string();
                    } else if (pattern_node.type == NodeType::NumberLit) {
                        pattern_key = "n:" + std::to_string(pattern_node.as_number());
                    } else if (pattern_node.type == NodeType::BoolLit) {
                        pattern_key = "b:" + std::to_string(pattern_node.as_bool());
                    }

                    if (pattern_key == scrutinee_key) {
                        // Pattern matches - check guard if present
                        bool guard_passes = true;
                        if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                            const Node& guard_node = ast_->arena[arm_data.guard_node];
                            if (guard_node.type == NodeType::BoolLit) {
                                guard_passes = guard_node.as_bool();
                            } else if (guard_node.type == NodeType::NumberLit) {
                                guard_passes = guard_node.as_number() != 0.0;
                            }
                        }

                        if (guard_passes && body != NULL_NODE) {
                            auto result_tv = visit(body);
                            return cache_and_return(node, result_tv);
                        }
                    }
                }
            } else {
                // Guard-only form: evaluate guard
                if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                    const Node& guard_node = ast_->arena[arm_data.guard_node];
                    bool guard_passes = false;
                    if (guard_node.type == NodeType::BoolLit) {
                        guard_passes = guard_node.as_bool();
                    } else if (guard_node.type == NodeType::NumberLit) {
                        guard_passes = guard_node.as_number() != 0.0;
                    }

                    if (guard_passes && body != NULL_NODE) {
                        auto result_tv = visit(body);
                        return cache_and_return(node, result_tv);
                    }
                }
            }
        }
        arm = ast_->arena[arm].next_sibling;
    }

    // No match found - use default if available
    if (default_body != NULL_NODE) {
        auto result_tv = visit(default_body);
        return cache_and_return(node, result_tv);
    }

    error("E121", "No matching pattern in match expression", n.location);
    return TypedValue::void_val();
}

// Handle runtime match - emit all branches and build nested select chain
TypedValue CodeGenerator::handle_runtime_match(NodeIndex node, const Node& n) {
    const auto& match_data = n.as_match_expr();

    // Check for missing wildcard arm and warn
    bool has_wildcard = false;
    NodeIndex first_arm = n.first_child;

    if (match_data.has_scrutinee) {
        first_arm = ast_->arena[first_arm].next_sibling;
    }

    NodeIndex arm = first_arm;
    while (arm != NULL_NODE) {
        const Node& arm_node = ast_->arena[arm];
        if (arm_node.type == NodeType::MatchArm) {
            const auto& arm_data = arm_node.as_match_arm();
            if (arm_data.is_wildcard || (arm_data.is_destructure && !arm_data.has_guard)) {
                has_wildcard = true;
                break;
            }
        }
        arm = ast_->arena[arm].next_sibling;
    }

    if (!has_wildcard) {
        warn("W001", "Match expression missing default '_' arm; defaulting to 0.0", n.location);
    }

    // Visit scrutinee if present
    std::uint16_t scrutinee_buf = BufferAllocator::BUFFER_UNUSED;
    if (match_data.has_scrutinee) {
        scrutinee_buf = visit(n.first_child).buffer;
    }

    // Collect all arms: condition buffer, body buffer, is_wildcard
    struct ArmInfo {
        std::uint16_t cond_buf;
        std::uint16_t body_buf;
        bool is_wildcard;
    };
    std::vector<ArmInfo> arms;

    arm = first_arm;
    while (arm != NULL_NODE) {
        const Node& arm_node = ast_->arena[arm];
        if (arm_node.type == NodeType::MatchArm) {
            const auto& arm_data = arm_node.as_match_arm();

            NodeIndex pattern = arm_node.first_child;
            NodeIndex body = (pattern != NULL_NODE) ?
                            ast_->arena[pattern].next_sibling : NULL_NODE;

            // Bind destructured fields before visiting body/guard (they may reference them)
            bool has_destr_scope = false;
            if (arm_data.is_destructure && match_data.has_scrutinee) {
                symbols_->push_scope();
                has_destr_scope = true;
                auto* scrutinee_type = get_node_type(n.first_child);
                if (scrutinee_type) {
                    // Match-arm destructure has no default expressions.
                    std::vector<DestructureField> arm_fields;
                    arm_fields.reserve(arm_data.destructure_fields.size());
                    for (const auto& fname : arm_data.destructure_fields) {
                        arm_fields.push_back(DestructureField{fname, NULL_NODE});
                    }
                    bind_destructure_fields(*scrutinee_type, arm_fields, arm_node.location);
                }
            }

            // Visit body first (all branches compute in DSP)
            std::uint16_t body_buf = BufferAllocator::BUFFER_UNUSED;
            if (body != NULL_NODE) {
                body_buf = visit(body).buffer;
            } else {
                // Empty body -> emit 0.0
                body_buf = buffers_.allocate();
                cedar::Instruction push_inst{};
                push_inst.opcode = cedar::Opcode::PUSH_CONST;
                push_inst.out_buffer = body_buf;
                push_inst.inputs[0] = 0xFFFF;
                push_inst.inputs[1] = 0xFFFF;
                push_inst.inputs[2] = 0xFFFF;
                push_inst.inputs[3] = 0xFFFF;
                codegen::encode_const_value(push_inst, 0.0f);
                emit(push_inst);
            }

            if (arm_data.is_wildcard) {
                if (has_destr_scope) symbols_->pop_scope();
                arms.push_back({BufferAllocator::BUFFER_UNUSED, body_buf, true});
            } else if (arm_data.is_destructure) {
                // Destructuring arm: always matches unless guarded
                if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                    std::uint16_t guard_buf = visit(arm_data.guard_node).buffer;
                    if (has_destr_scope) symbols_->pop_scope();
                    arms.push_back({guard_buf, body_buf, false});
                } else {
                    // No guard → always matches (like wildcard)
                    if (has_destr_scope) symbols_->pop_scope();
                    arms.push_back({BufferAllocator::BUFFER_UNUSED, body_buf, true});
                }
            } else {
                // Build condition
                std::uint16_t cond_buf = BufferAllocator::BUFFER_UNUSED;

                if (match_data.has_scrutinee && arm_data.is_range) {
                    // Range pattern: scrutinee >= low AND scrutinee < high
                    // Emit PUSH_CONST for low bound
                    std::uint16_t low_buf = buffers_.allocate();
                    cedar::Instruction push_low{};
                    push_low.opcode = cedar::Opcode::PUSH_CONST;
                    push_low.out_buffer = low_buf;
                    push_low.inputs[0] = 0xFFFF;
                    push_low.inputs[1] = 0xFFFF;
                    push_low.inputs[2] = 0xFFFF;
                    push_low.inputs[3] = 0xFFFF;
                    codegen::encode_const_value(push_low, static_cast<float>(arm_data.range_low));
                    emit(push_low);

                    // CMP_GTE: scrutinee >= low
                    std::uint16_t gte_buf = buffers_.allocate();
                    cedar::Instruction gte_inst{};
                    gte_inst.opcode = cedar::Opcode::CMP_GTE;
                    gte_inst.out_buffer = gte_buf;
                    gte_inst.inputs[0] = scrutinee_buf;
                    gte_inst.inputs[1] = low_buf;
                    gte_inst.inputs[2] = 0xFFFF;
                    gte_inst.inputs[3] = 0xFFFF;
                    emit(gte_inst);

                    // Emit PUSH_CONST for high bound
                    std::uint16_t high_buf = buffers_.allocate();
                    cedar::Instruction push_high{};
                    push_high.opcode = cedar::Opcode::PUSH_CONST;
                    push_high.out_buffer = high_buf;
                    push_high.inputs[0] = 0xFFFF;
                    push_high.inputs[1] = 0xFFFF;
                    push_high.inputs[2] = 0xFFFF;
                    push_high.inputs[3] = 0xFFFF;
                    codegen::encode_const_value(push_high, static_cast<float>(arm_data.range_high));
                    emit(push_high);

                    // CMP_LT: scrutinee < high
                    std::uint16_t lt_buf = buffers_.allocate();
                    cedar::Instruction lt_inst{};
                    lt_inst.opcode = cedar::Opcode::CMP_LT;
                    lt_inst.out_buffer = lt_buf;
                    lt_inst.inputs[0] = scrutinee_buf;
                    lt_inst.inputs[1] = high_buf;
                    lt_inst.inputs[2] = 0xFFFF;
                    lt_inst.inputs[3] = 0xFFFF;
                    emit(lt_inst);

                    // LOGIC_AND: both conditions
                    cond_buf = buffers_.allocate();
                    cedar::Instruction and_inst{};
                    and_inst.opcode = cedar::Opcode::LOGIC_AND;
                    and_inst.out_buffer = cond_buf;
                    and_inst.inputs[0] = gte_buf;
                    and_inst.inputs[1] = lt_buf;
                    and_inst.inputs[2] = 0xFFFF;
                    and_inst.inputs[3] = 0xFFFF;
                    emit(and_inst);
                } else if (match_data.has_scrutinee) {
                    // Scrutinee form: eq(scrutinee, pattern)
                    std::uint16_t pattern_buf = visit(pattern).buffer;

                    cond_buf = buffers_.allocate();
                    cedar::Instruction eq_inst{};
                    eq_inst.opcode = cedar::Opcode::CMP_EQ;
                    eq_inst.out_buffer = cond_buf;
                    eq_inst.inputs[0] = scrutinee_buf;
                    eq_inst.inputs[1] = pattern_buf;
                    eq_inst.inputs[2] = 0xFFFF;
                    eq_inst.inputs[3] = 0xFFFF;
                    emit(eq_inst);
                } else {
                    // Guard-only form: condition is the guard itself
                    if (arm_data.has_guard && arm_data.guard_node != NULL_NODE) {
                        cond_buf = visit(arm_data.guard_node).buffer;
                    }
                }

                // If there's a guard, AND it with the pattern condition
                if (arm_data.has_guard && arm_data.guard_node != NULL_NODE && match_data.has_scrutinee) {
                    std::uint16_t guard_buf = visit(arm_data.guard_node).buffer;

                    std::uint16_t combined_buf = buffers_.allocate();
                    cedar::Instruction and_inst{};
                    and_inst.opcode = cedar::Opcode::LOGIC_AND;
                    and_inst.out_buffer = combined_buf;
                    and_inst.inputs[0] = cond_buf;
                    and_inst.inputs[1] = guard_buf;
                    and_inst.inputs[2] = 0xFFFF;
                    and_inst.inputs[3] = 0xFFFF;
                    emit(and_inst);
                    cond_buf = combined_buf;
                }

                arms.push_back({cond_buf, body_buf, false});
            }
        }
        arm = ast_->arena[arm].next_sibling;
    }

    if (arms.empty()) {
        error("E122", "Match expression has no arms", n.location);
        return TypedValue::void_val();
    }

    // Build nested select chain (reverse order)
    // Start with default (wildcard arm or 0.0)
    std::uint16_t result = BufferAllocator::BUFFER_UNUSED;

    // Find wildcard arm for default
    for (const auto& arm_info : arms) {
        if (arm_info.is_wildcard) {
            result = arm_info.body_buf;
            break;
        }
    }

    // If no wildcard, emit 0.0 as default
    if (result == BufferAllocator::BUFFER_UNUSED) {
        result = buffers_.allocate();
        cedar::Instruction push_inst{};
        push_inst.opcode = cedar::Opcode::PUSH_CONST;
        push_inst.out_buffer = result;
        push_inst.inputs[0] = 0xFFFF;
        push_inst.inputs[1] = 0xFFFF;
        push_inst.inputs[2] = 0xFFFF;
        push_inst.inputs[3] = 0xFFFF;
        codegen::encode_const_value(push_inst, 0.0f);
        emit(push_inst);
    }

    // Build select chain in reverse order (last non-wildcard arm first)
    for (auto it = arms.rbegin(); it != arms.rend(); ++it) {
        if (!it->is_wildcard && it->cond_buf != BufferAllocator::BUFFER_UNUSED) {
            std::uint16_t select_buf = buffers_.allocate();
            cedar::Instruction sel_inst{};
            sel_inst.opcode = cedar::Opcode::SELECT;
            sel_inst.out_buffer = select_buf;
            sel_inst.inputs[0] = it->cond_buf;   // condition
            sel_inst.inputs[1] = it->body_buf;   // true branch
            sel_inst.inputs[2] = result;         // false branch (previous result)
            sel_inst.inputs[3] = 0xFFFF;
            emit(sel_inst);
            result = select_buf;
        }
    }

    return cache_and_return(node, TypedValue::signal(result));
}

// Handle MatchExpr nodes - dispatch to compile-time or runtime handling
TypedValue CodeGenerator::handle_match_expr(NodeIndex node, const Node& n) {
    // Check if match expression data exists
    if (!std::holds_alternative<Node::MatchExprData>(n.data)) {
        // Legacy: no MatchExprData, assume scrutinee form without guards
        // Fall back to compile-time behavior for backwards compatibility
        return handle_compile_time_match(node, n);
    }

    if (is_compile_time_match(node, n)) {
        return handle_compile_time_match(node, n);
    } else {
        return handle_runtime_match(node, n);
    }
}

// Handle tap_delay(in, time, fb, processor) - tap delay with inline feedback chain
// Also handles tap_delay_ms and tap_delay_smp variants with different time units.
// Emits DELAY_TAP, compiles processor closure inline, then emits DELAY_WRITE
// Both opcodes share the same state_id to operate on the same delay buffer
TypedValue CodeGenerator::handle_tap_delay_call(NodeIndex node, const Node& n) {
    // Extract function name to determine time unit
    const std::string& func_name = n.as_identifier();

    // Determine time unit from function name
    // 0 = seconds (default), 1 = milliseconds, 2 = samples
    std::uint8_t time_unit = 0;
    if (func_name == "tap_delay_ms") {
        time_unit = 1;
    } else if (func_name == "tap_delay_smp") {
        time_unit = 2;
    }

    // Collect arguments: in, time, fb, processor
    std::vector<NodeIndex> args;
    NodeIndex arg = n.first_child;
    while (arg != NULL_NODE) {
        const Node& arg_node = ast_->arena[arg];
        NodeIndex arg_value = arg;
        if (arg_node.type == NodeType::Argument) {
            arg_value = arg_node.first_child;
        }
        args.push_back(arg_value);
        arg = ast_->arena[arg].next_sibling;
    }

    if (args.size() < 4) {
        error("E301", func_name + "() requires 4 arguments: " + func_name + "(in, time, fb, processor)", n.location);
        return TypedValue::void_val();
    }

    // Validate that processor (4th arg) is a Closure
    const Node& processor_node = ast_->arena[args[3]];
    if (processor_node.type != NodeType::Closure) {
        error("E302", "tap_delay() 4th argument must be a closure: (x) -> ...", n.location);
        return TypedValue::void_val();
    }

    // Extract closure parameter (must have exactly 1 parameter)
    std::string param_name;
    NodeIndex closure_body = NULL_NODE;
    NodeIndex child = processor_node.first_child;
    int param_count = 0;

    while (child != NULL_NODE) {
        const Node& child_node = ast_->arena[child];
        if (child_node.type == NodeType::Identifier) {
            if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                param_name = child_node.as_closure_param().name;
                param_count++;
            } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                param_name = child_node.as_identifier();
                param_count++;
            } else {
                // Not a parameter - this is the body
                closure_body = child;
                break;
            }
        } else {
            // Body node
            closure_body = child;
            break;
        }
        child = ast_->arena[child].next_sibling;
    }

    if (param_count != 1) {
        error("E303", "tap_delay() processor closure must have exactly 1 parameter", n.location);
        return TypedValue::void_val();
    }

    if (closure_body == NULL_NODE) {
        error("E304", "tap_delay() processor closure has no body", n.location);
        return TypedValue::void_val();
    }

    // Push path for semantic ID tracking (before TAP)
    std::uint32_t count = call_counters_["tap_delay"]++;
    std::string unique_name = "tap_delay#" + std::to_string(count);
    push_path(unique_name);

    // Compute shared state_id for TAP and WRITE
    std::uint32_t delay_state_id = compute_state_id();

    // Visit input arguments (in, time, fb)
    std::uint16_t in_buf = visit(args[0]).buffer;
    std::uint16_t time_buf = visit(args[1]).buffer;
    std::uint16_t fb_buf = visit(args[2]).buffer;

    // Handle optional dry/wet arguments (args[4], args[5])
    // Default: dry=0.0, wet=1.0 (100% wet, backward compatible)
    std::uint16_t dry_buf = BufferAllocator::BUFFER_UNUSED;
    std::uint16_t wet_buf = BufferAllocator::BUFFER_UNUSED;

    if (args.size() > 4) {
        dry_buf = visit(args[4]).buffer;
    }
    if (args.size() > 5) {
        wet_buf = visit(args[5]).buffer;
    }

    // Allocate output buffer for DELAY_TAP
    std::uint16_t tap_out_buf = buffers_.allocate();
    if (tap_out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // Emit DELAY_TAP instruction
    // DELAY_TAP: reads from delay buffer, outputs delayed signal
    // in0: input (passed through for signal flow), in1: delay time
    // rate field: time unit (0=seconds, 1=ms, 2=samples)
    cedar::Instruction tap_inst{};
    tap_inst.opcode = cedar::Opcode::DELAY_TAP;
    tap_inst.out_buffer = tap_out_buf;
    tap_inst.inputs[0] = in_buf;
    tap_inst.inputs[1] = time_buf;
    tap_inst.inputs[2] = 0xFFFF;
    tap_inst.inputs[3] = 0xFFFF;
    tap_inst.inputs[4] = 0xFFFF;
    tap_inst.rate = time_unit;
    tap_inst.state_id = delay_state_id;
    emit(tap_inst);

    // Push scope and bind closure parameter to TAP output
    symbols_->push_scope();
    symbols_->define_variable(param_name, tap_out_buf);

    // Push semantic context for nested opcodes
    push_path("fb");

    // Save node_types_ state before visiting closure body
    auto saved_node_types = std::move(node_types_);
    node_types_.clear();

    // Compile the closure body (feedback chain)
    std::uint16_t processed_buf = visit(closure_body).buffer;

    // Restore node_types_
    node_types_ = std::move(saved_node_types);

    // Pop semantic context
    pop_path();

    // Pop scope
    symbols_->pop_scope();

    // Allocate output buffer for DELAY_WRITE
    std::uint16_t write_out_buf = buffers_.allocate();
    if (write_out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // Emit DELAY_WRITE instruction
    // DELAY_WRITE: writes input + processed*fb to buffer, outputs delayed signal
    // in0: input, in1: processed feedback, in2: feedback amount
    // in3: dry level (0xFFFF = default 0.0), in4: wet level (0xFFFF = default 1.0)
    // Note: DELAY_WRITE doesn't need rate field (time conversion done by DELAY_TAP)
    cedar::Instruction write_inst{};
    write_inst.opcode = cedar::Opcode::DELAY_WRITE;
    write_inst.out_buffer = write_out_buf;
    write_inst.inputs[0] = in_buf;
    write_inst.inputs[1] = processed_buf;
    write_inst.inputs[2] = fb_buf;
    write_inst.inputs[3] = dry_buf;
    write_inst.inputs[4] = wet_buf;
    write_inst.rate = 0;
    write_inst.state_id = delay_state_id;  // Same state_id as TAP!
    emit(write_inst);

    // Pop the outer path
    pop_path();

    return cache_and_return(node, TypedValue::signal(write_out_buf));
}

// Handle poly(voices, instrument) / mono(instrument) / legato(instrument)
// Emits POLY_BEGIN, inlines instrument body, emits POLY_END
TypedValue CodeGenerator::handle_poly_call(NodeIndex node, const Node& n) {
    using codegen::extract_call_args;

    const std::string& func_name = n.as_identifier();

    // Determine mode from function name
    std::uint8_t mode = 0;  // 0=poly
    if (func_name == "mono") mode = 1;
    else if (func_name == "legato") mode = 2;

    // Collect arguments:
    //   poly: 2 required (input, instrument), 1 optional (voices, default 64)
    //   mono/legato: 1-2 (instrument) or (input, instrument)
    auto args = extract_call_args(ast_->arena, n.first_child,
                                  mode == 0 ? 2 : 1,
                                  mode == 0 ? 3 : 2);
    if (!args.valid) {
        if (mode == 0) {
            error("E400", "poly() requires 2-3 arguments: poly(input, instrument, voices=64). Pipe a pattern in: pat(...) |> poly(%, instrument)", n.location);
        } else {
            error("E400", func_name + "() requires 1-2 arguments: " + func_name + "(instrument) or " + func_name + "(input, instrument)", n.location);
        }
        return TypedValue::void_val();
    }

    // Parse arguments based on func_name and arg count
    NodeIndex pattern_arg = NULL_NODE;
    NodeIndex voices_arg = NULL_NODE;
    NodeIndex instrument_arg = NULL_NODE;
    std::uint8_t max_voices = (mode == 0) ? 64 : 1;

    if (mode == 0) {
        // poly: (input, instrument, voices=64)
        pattern_arg = args.nodes[0];
        instrument_arg = args.nodes[1];
        if (args.nodes.size() == 3) {
            voices_arg = args.nodes[2];
        }
    } else {
        // mono/legato: 1 arg = (fn), 2 args = (input, fn)
        if (args.nodes.size() == 2) {
            pattern_arg = args.nodes[0];
            instrument_arg = args.nodes[1];
        } else if (args.nodes.size() == 1) {
            instrument_arg = args.nodes[0];
        } else {
            error("E400", func_name + "() requires 1-2 arguments", n.location);
            return TypedValue::void_val();
        }
    }

    // Extract voice count from number literal (poly only)
    if (voices_arg != NULL_NODE) {
        const Node& vn = ast_->arena[voices_arg];
        if (vn.type == NodeType::NumberLit) {
            int v = static_cast<int>(vn.as_number());
            if (v < 1 || v > 128) {
                error("E401", "Voice count must be between 1 and 128", vn.location);
                return TypedValue::void_val();
            }
            max_voices = static_cast<std::uint8_t>(v);
        } else {
            error("E402", "Voice count must be a number literal", vn.location);
            return TypedValue::void_val();
        }
    }

    // Visit pattern input if present (emits SEQPAT_QUERY etc.)
    std::uint32_t seq_state_id = 0;
    if (pattern_arg != NULL_NODE) {
        auto pat_tv = visit(pattern_arg);
        // Look up pattern state_id from TypedValue
        if (pat_tv.pattern) {
            seq_state_id = pat_tv.pattern->state_id;
        }
        // Consume polyphonic tracking — poly() handles voice allocation at runtime
        polyphonic_pattern_nodes_.erase(pattern_arg);
    }

    // Resolve instrument function
    auto func_ref = resolve_function_arg(instrument_arg);
    if (!func_ref) {
        error("E403", func_name + "() instrument argument must be a function with 3 parameters (freq, gate, vel)", n.location);
        return TypedValue::void_val();
    }

    // Validate exactly 3 parameters
    if (func_ref->params.size() != 3) {
        error("E404", func_name + "() instrument function must have exactly 3 parameters (freq, gate, vel), got " +
              std::to_string(func_ref->params.size()), n.location);
        return TypedValue::void_val();
    }

    // Allocate scratch buffers for voice parameters and output
    std::uint16_t voice_freq_buf = buffers_.allocate();
    std::uint16_t voice_gate_buf = buffers_.allocate();
    std::uint16_t voice_vel_buf = buffers_.allocate();
    std::uint16_t voice_trig_buf = buffers_.allocate();
    std::uint16_t voice_out_buf = buffers_.allocate();
    std::uint16_t mix_buf = buffers_.allocate();

    if (mix_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }

    // Push semantic path for unique state_id
    std::uint32_t count = call_counters_["poly"]++;
    push_path("poly#" + std::to_string(count));
    std::uint32_t poly_state_id = compute_state_id();

    // Emit POLY_BEGIN instruction
    std::size_t poly_begin_idx = instructions_.size();
    cedar::Instruction poly_begin{};
    poly_begin.opcode = cedar::Opcode::POLY_BEGIN;
    poly_begin.out_buffer = mix_buf;
    poly_begin.inputs[0] = voice_freq_buf;
    poly_begin.inputs[1] = voice_gate_buf;
    poly_begin.inputs[2] = voice_vel_buf;
    poly_begin.inputs[3] = voice_trig_buf;
    poly_begin.inputs[4] = voice_out_buf;
    poly_begin.rate = 0;  // Patched after body emission
    poly_begin.state_id = poly_state_id;
    emit(poly_begin);

    // Inline function body
    // Push scope and bind params to voice buffers
    symbols_->push_scope();
    symbols_->define_variable(func_ref->params[0].name, voice_freq_buf);
    symbols_->define_variable(func_ref->params[1].name, voice_gate_buf);
    symbols_->define_variable(func_ref->params[2].name, voice_vel_buf);

    // Bind captures for closures
    for (const auto& capture : func_ref->captures) {
        symbols_->define_variable(capture.name, capture.buffer_index);
    }

    // Save node_types_ state before visiting body
    auto saved_node_types = std::move(node_types_);
    node_types_.clear();

    // Record body start instruction index
    std::size_t body_start = instructions_.size();

    // Visit body
    std::uint16_t body_result = BufferAllocator::BUFFER_UNUSED;
    if (func_ref->is_user_function) {
        // User function: body is directly the body_node
        body_result = visit(func_ref->closure_node).buffer;
    } else {
        // Closure: find body (last child of closure node)
        const Node& closure_node = ast_->arena[func_ref->closure_node];
        NodeIndex child = closure_node.first_child;
        NodeIndex body = NULL_NODE;
        while (child != NULL_NODE) {
            const Node& child_node = ast_->arena[child];
            if (child_node.type == NodeType::Identifier) {
                // parameter — skip
            } else {
                body = child;
                break;
            }
            child = ast_->arena[child].next_sibling;
        }
        if (body != NULL_NODE) {
            body_result = visit(body).buffer;
        }
    }

    // If body result != voice_out_buf, emit COPY to wire it
    if (body_result != BufferAllocator::BUFFER_UNUSED && body_result != voice_out_buf) {
        cedar::Instruction copy_inst{};
        copy_inst.opcode = cedar::Opcode::COPY;
        copy_inst.out_buffer = voice_out_buf;
        copy_inst.inputs[0] = body_result;
        copy_inst.inputs[1] = 0xFFFF;
        copy_inst.inputs[2] = 0xFFFF;
        copy_inst.inputs[3] = 0xFFFF;
        copy_inst.inputs[4] = 0xFFFF;
        copy_inst.state_id = 0;
        emit(copy_inst);
    }

    // Record body end
    std::size_t body_end = instructions_.size();

    // Restore node_types_
    for (auto& [k, v] : saved_node_types) {
        if (node_types_.find(k) == node_types_.end()) {
            node_types_[k] = v;
        }
    }

    // Pop scope
    symbols_->pop_scope();

    // Patch POLY_BEGIN.rate = body_length
    std::size_t body_length = body_end - body_start;
    if (body_length > 255) {
        error("E405", "Poly instrument body too large (max 255 instructions)", n.location);
        pop_path();
        return TypedValue::void_val();
    }
    instructions_[poly_begin_idx].rate = static_cast<std::uint8_t>(body_length);

    // Emit POLY_END
    cedar::Instruction poly_end{};
    poly_end.opcode = cedar::Opcode::POLY_END;
    poly_end.out_buffer = 0xFFFF;
    poly_end.inputs[0] = 0xFFFF;
    poly_end.inputs[1] = 0xFFFF;
    poly_end.inputs[2] = 0xFFFF;
    poly_end.inputs[3] = 0xFFFF;
    poly_end.inputs[4] = 0xFFFF;
    poly_end.state_id = 0;
    emit(poly_end);

    // Pop semantic path
    pop_path();

    // Add StateInitData for PolyAlloc
    StateInitData poly_init;
    poly_init.state_id = poly_state_id;
    poly_init.type = StateInitData::Type::PolyAlloc;
    poly_init.poly_seq_state_id = seq_state_id;
    poly_init.poly_max_voices = max_voices;
    poly_init.poly_mode = mode;
    poly_init.poly_steal_strategy = 0;  // oldest
    state_inits_.push_back(std::move(poly_init));

    return cache_and_return(node, TypedValue::signal(mix_buf));
}

} // namespace akkado
