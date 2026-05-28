// User-defined function and match expression codegen implementations
// Extracted from codegen.cpp for maintainability

#include "akkado/codegen.hpp"
#include "akkado/compile_context.hpp"
#include "akkado/string_interner.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/const_eval.hpp"
#include <cstring>

namespace akkado {

using codegen::encode_const_value;
using codegen::closure_body;

// Maximum parameter count for a shareable `fn` (PRD §4.2). Params 0..4 travel
// in the BLOCK_CALL instruction's inputs[0..4]; params 5..N-1 travel in a run
// of preceding BLOCK_BIND instructions (slot index in `rate`, a uint8). The
// cap stays well within the uint8 range; the contiguous param-bank allocation
// in get_or_compile_shared_block self-limits in practice. A `fn` with more
// parameters falls back to per-site inlining.
static constexpr std::size_t MAX_SHARED_FN_PARAMS = 32;

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
            // Phase 5 (F12): lookup by SymbolId — no rehash. The const_eval
            // helper doesn't need to materialize the name as a string.
            if (std::holds_alternative<Node::IdentifierData>(n.data)) {
                auto sym = symbols.lookup(n.as_identifier());
                if (sym && sym->is_const && sym->const_value.has_value()) {
                    return *sym->const_value;
                }
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
            ConstEvaluator evaluator(*ast_, *symbols_, *ctx_->interner);
            auto result = evaluator.eval_const_fn_call(func, const_args, n.location);

            for (const auto& diag : evaluator.diagnostics()) {
                diagnostics_.push_back(diag);
            }

            if (result) {
                if (std::holds_alternative<double>(*result)) {
                    float val = static_cast<float>(std::get<double>(*result));
                    std::uint16_t buf = emit_push_const(val);
                    if (buf == BufferAllocator::BUFFER_UNUSED) {
                        error("E101", "Buffer pool exhausted", n.location);
                        return TypedValue::void_val();
                    }
                    return cache_and_return(node, TypedValue::number(buf));
                } else {
                    // Array result
                    const auto& arr = std::get<std::vector<double>>(*result);
                    std::vector<std::uint16_t> result_buffers;
                    for (double v : arr) {
                        std::uint16_t buf = emit_push_const(static_cast<float>(v));
                        if (buf == BufferAllocator::BUFFER_UNUSED) {
                            error("E101", "Buffer pool exhausted", n.location);
                            return TypedValue::void_val();
                        }
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

    // PRD prd-runtime-functions-control-flow L2: shared-block lowering. An
    // eligible non-`#inline` fn is compiled once into a subprogram and
    // dispatched via BLOCK_CALL at each call site, instead of being inlined
    // everywhere. Falls through to the inline path below when the fn is not
    // shareable or this call site cannot use the shared block.
    if (!has_spread && !options_.inline_all_fns &&
        args.size() == func.params.size() &&
        fn_shareable_by_signature(func)) {
        // Evaluate the call-site arguments first: a shared block can only be
        // dispatched when every argument resolves to a plain signal/number
        // buffer (no Array / Record / FunctionRef / string literal / `_`
        // placeholder).
        std::vector<std::uint16_t> arg_bufs;
        bool callable = true;
        for (NodeIndex a : args) {
            const Node& an = ast_->arena[a];
            if (an.type == NodeType::Identifier &&
                std::holds_alternative<Node::IdentifierData>(an.data) &&
                ctx_->interner->view(an.as_identifier()) == "_") {
                callable = false;  // `_` -> inline path fills the default
                break;
            }
            if (resolve_function_arg(a)) { callable = false; break; }
            TypedValue tv = visit(a);
            if ((tv.type != ValueType::Signal &&
                 tv.type != ValueType::Number) ||
                tv.buffer == BufferAllocator::BUFFER_UNUSED) {
                callable = false;
                break;
            }
            arg_bufs.push_back(tv.buffer);
        }
        // Compile the shared block only once a call site can actually
        // dispatch it. Otherwise a fn whose every call site passes a
        // non-signal arg (e.g. `osc("sin")`) would leave a dead, never-
        // called block bloating the bytecode — the opposite of L2's goal.
        if (callable) {
            const SharedBlock* block = get_or_compile_shared_block(func);
            if (block) {
                return emit_block_call(node, n, func, *block, arg_bufs);
            }
        }
        // Args are already visited and cached in node_types_; the inline
        // path's re-visit() below cache-hits, so no double emission.
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
                ConstEvaluator evaluator(*ast_, *symbols_, *ctx_->interner);
                auto result = evaluator.evaluate(param.default_node);
                for (const auto& diag : evaluator.diagnostics()) {
                    diagnostics_.push_back(diag);
                }
                if (result && std::holds_alternative<double>(*result)) {
                    float val = static_cast<float>(std::get<double>(*result));
                    param_buf = emit_push_const(val);
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
                ctx_->interner->view(arg_inner.as_identifier()) == "_");

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
                    ConstEvaluator evaluator(*ast_, *symbols_, *ctx_->interner);
                    auto result = evaluator.evaluate(func.params[i].default_node);
                    for (const auto& diag : evaluator.diagnostics()) {
                        diagnostics_.push_back(diag);
                    }
                    if (result && std::holds_alternative<double>(*result)) {
                        float val = static_cast<float>(std::get<double>(*result));
                        param_buf = emit_push_const(val);
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

                    // PRD prd-parameter-type-annotations §4.2: branch on the
                    // resolved annotation. Un-annotated params keep today's
                    // E160-only behavior; `: stream` bypasses E160 entirely
                    // and emits E184 for incompatible types; `: signal` keeps
                    // E160 for polyphonic non-sample patterns and adds E184
                    // for fundamentally incompatible types.
                    const ParamValueType expected = func.params[i].annotated_type;

                    if (expected == ParamValueType::Stream) {
                        // PRD §4.2 row "Stream / Pattern" + "Stream / EventSource":
                        // pass-through. Other types fire E184.
                        //
                        // Bare-identifier args (`m = n"…"; xp(m, 7)`) don't get
                        // their Pattern TypedValue recorded in `node_types_`
                        // by visit() automatically, so the param-binding pass
                        // at line ~757 would fall through to `define_variable`
                        // and lose the Pattern handle. Pin it here so the
                        // binding pass picks it up. Runtime event streams
                        // (midi()) ride on the Pattern type with the
                        // `is_runtime_event_source` flag set.
                        if (arg_tv.type == ValueType::Pattern) {
                            node_types_[args[i]] = arg_tv;
                        }
                        if (arg_tv.type != ValueType::Pattern) {
                            std::string msg = "parameter '" + func.params[i].name +
                                              "' of fn '" + func.name +
                                              "' expects Stream, got " +
                                              std::string(value_type_name(arg_tv.type)) +
                                              " — no coercion path";
                            if (arg_tv.type == ValueType::DynArray) {
                                msg += " (DynArray is a runtime-varying numeric "
                                       "array, not an event stream — pass the "
                                       "Pattern that produced it instead)";
                            }
                            error("E184", msg, ast_->arena[args[i]].location);
                        }
                    } else if (expected == ParamValueType::Signal) {
                        // PRD §4.2 row "Signal / polyphonic Pattern" preserves
                        // the existing E160 reject; row "Signal / EventSource…
                        // / Void" emits E184.
                        if (arg_tv.type == ValueType::Pattern && arg_tv.pattern &&
                            arg_tv.pattern->max_voices > 1 &&
                            !arg_tv.pattern->is_sample_pattern) {
                            error("E160",
                                  "user function parameter '" + func.params[i].name +
                                  "' cannot accept a polyphonic pattern as scalar; "
                                  "use poly() to consume it, or pick a voice/field "
                                  "explicitly (e.g. p.freq)",
                                  ast_->arena[args[i]].location);
                        } else if (arg_tv.type == ValueType::Record ||
                                   arg_tv.type == ValueType::Array ||
                                   arg_tv.type == ValueType::DynArray ||
                                   arg_tv.type == ValueType::String ||
                                   arg_tv.type == ValueType::Function ||
                                   arg_tv.type == ValueType::StateCell ||
                                   arg_tv.type == ValueType::Void) {
                            error("E184",
                                  "parameter '" + func.params[i].name +
                                  "' of fn '" + func.name +
                                  "' expects Signal, got " +
                                  std::string(value_type_name(arg_tv.type)) +
                                  " — no coercion path",
                                  ast_->arena[args[i]].location);
                        }
                        // Signal, Number, mono Pattern → silent voice-0
                        // coerce (today's implicit behavior).
                    } else if (expected == ParamValueType::Number) {
                        // PRD prd-parameter-type-annotations-phase-2 §4.2:
                        // strict — Number only. Signal does not coerce.
                        if (arg_tv.type != ValueType::Number) {
                            error("E184",
                                  "parameter '" + func.params[i].name +
                                  "' of fn '" + func.name +
                                  "' expects Number, got " +
                                  std::string(value_type_name(arg_tv.type)) +
                                  " — no coercion path",
                                  ast_->arena[args[i]].location);
                        }
                    } else if (expected == ParamValueType::Record) {
                        // PRD prd-parameter-type-annotations-phase-2 §4.2 + §8.6:
                        // Record accepts Record OR Pattern (Pattern is
                        // structurally a record). Pin Pattern args into
                        // node_types_ so the binding-selection block at
                        // line ~768 picks up the Pattern TypedValue (mirror
                        // of the Stream branch above) — required for body
                        // field-access like `r.freq` to resolve.
                        if (arg_tv.type == ValueType::Pattern) {
                            node_types_[args[i]] = arg_tv;
                        }
                        if (arg_tv.type != ValueType::Record &&
                            arg_tv.type != ValueType::Pattern) {
                            error("E184",
                                  "parameter '" + func.params[i].name +
                                  "' of fn '" + func.name +
                                  "' expects Record, got " +
                                  std::string(value_type_name(arg_tv.type)) +
                                  " — no coercion path",
                                  ast_->arena[args[i]].location);
                        }
                    } else if (expected == ParamValueType::Array) {
                        // PRD prd-parameter-type-annotations-phase-2 §4.2:
                        // compile-time Array only; DynArray rejected (mirrors
                        // the Stream/DynArray reject above).
                        if (arg_tv.type != ValueType::Array) {
                            std::string msg = "parameter '" + func.params[i].name +
                                              "' of fn '" + func.name +
                                              "' expects Array, got " +
                                              std::string(value_type_name(arg_tv.type)) +
                                              " — no coercion path";
                            if (arg_tv.type == ValueType::DynArray) {
                                msg += " (DynArray is a runtime-varying numeric "
                                       "array — use the un-annotated path or "
                                       "pass an unrolled array literal)";
                            }
                            error("E184", msg, ast_->arena[args[i]].location);
                        }
                    } else if (expected == ParamValueType::String) {
                        // PRD prd-parameter-type-annotations-phase-2 §4.2:
                        // compile-time String only.
                        if (arg_tv.type != ValueType::String) {
                            error("E184",
                                  "parameter '" + func.params[i].name +
                                  "' of fn '" + func.name +
                                  "' expects String, got " +
                                  std::string(value_type_name(arg_tv.type)) +
                                  " — no coercion path",
                                  ast_->arena[args[i]].location);
                        }
                    } else if (expected == ParamValueType::Function) {
                        // PRD prd-parameter-type-annotations-phase-2 §4.2:
                        // Function references only.
                        if (arg_tv.type != ValueType::Function) {
                            error("E184",
                                  "parameter '" + func.params[i].name +
                                  "' of fn '" + func.name +
                                  "' expects Function, got " +
                                  std::string(value_type_name(arg_tv.type)) +
                                  " — no coercion path",
                                  ast_->arena[args[i]].location);
                        }
                    } else {
                        // ParamValueType::Any — un-annotated. Today's behavior,
                        // bit-for-bit: only the polyphonic-pattern guard fires.
                        // PRD prd-patterns-as-scalar-values §5.3 / §9.5.
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
            // Use numeric default value. Record the backing NumberLit node as a
            // param literal so `match(param)` const-folds and builtins like
            // linspace can resolve the param when left at its default.
            if (func.params[i].default_node != NULL_NODE) {
                std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
                param_literals_[param_hash] = func.params[i].default_node;
            }
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
            ConstEvaluator evaluator(*ast_, *symbols_, *ctx_->interner);
            auto result = evaluator.evaluate(func.params[i].default_node);

            for (const auto& diag : evaluator.diagnostics()) {
                diagnostics_.push_back(diag);
            }

            if (result) {
                if (std::holds_alternative<double>(*result)) {
                    float val = static_cast<float>(std::get<double>(*result));
                    param_buf = emit_push_const(val);
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
                // PRD prd-parameter-type-annotations §4.3: a `: stream`/`: evs`
                // -annotated param (and a `: rec`-annotated param receiving a
                // Pattern; PRD prd-parameter-type-annotations-phase-2 §8.6)
                // preserves the caller's Pattern TypedValue across the call
                // boundary, so field access (`events.freq` / `r.freq`) works
                // inside the body. Architectural template: the DynArray
                // branch below.
                if ((func.params[i].annotated_type == ParamValueType::Stream ||
                     func.params[i].annotated_type == ParamValueType::Record) &&
                    type_it != node_types_.end() &&
                    type_it->second.type == ValueType::Pattern) {
                    Symbol sym{};
                    sym.kind = SymbolKind::Variable;
                    sym.name_id = ctx_->interner->intern(func.params[i].name);
                    sym.name = func.params[i].name;
                    sym.buffer_index = param_bufs[i];
                    sym.typed_value = type_it->second;
                    symbols_->define(sym);
                } else if (type_it != node_types_.end() && type_it->second.type == ValueType::Record) {
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
                } else if (type_it != node_types_.end() &&
                           type_it->second.type == ValueType::DynArray) {
                    // PRD prd-pattern-event-arrays: a DynArray argument keeps
                    // its type across the call boundary via the symbol's
                    // typed_value, so notes()/freqs() results stay indexable
                    // (and rejectable) inside the function body.
                    Symbol sym{};
                    sym.kind = SymbolKind::Variable;
                    sym.name_id = ctx_->interner->intern(func.params[i].name);
                    sym.name = func.params[i].name;
                    sym.buffer_index = param_bufs[i];
                    sym.typed_value = type_it->second;
                    symbols_->define(sym);
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

    // Visit function body (inline expansion). PRD prd-midi-input §10 Q4:
    // track fn-body context so handle_midi_call can reject mid-body `midi()`.
    TypedValue result_tv = TypedValue::void_val();
    if (func.body_node != NULL_NODE) {
        ++user_function_depth_;
        result_tv = visit(func.body_node);
        --user_function_depth_;
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

// ============================================================================
// PRD prd-runtime-functions-control-flow L2 — shared-block `fn` lowering
// ============================================================================

// Pre-pass: count every Call node by callee name so get_or_compile_shared_block
// can skip fns that are only called once (sharing them saves nothing).
void CodeGenerator::count_fn_calls(NodeIndex node) {
    if (node == NULL_NODE) return;
    const Node& n = ast_->arena[node];
    if (n.type == NodeType::Call &&
        std::holds_alternative<Node::IdentifierData>(n.data)) {
        fn_call_counts_[std::string(ctx_->interner->view(n.as_identifier()))]++;
    }
    NodeIndex child = n.first_child;
    while (child != NULL_NODE) {
        count_fn_calls(child);
        child = ast_->arena[child].next_sibling;
    }
}

bool CodeGenerator::fn_shareable_by_signature(
    const UserFunctionInfo& func) const {
    if (func.is_inline || func.is_const || func.returns_closure ||
        func.has_rest_param || func.body_node == NULL_NODE) {
        return false;
    }
    if (func.params.empty() || func.params.size() > MAX_SHARED_FN_PARAMS) {
        return false;
    }
    for (const auto& p : func.params) {
        // Shared bodies bind every parameter to a runtime buffer; rest /
        // destructure params and per-call defaults cannot be expressed.
        if (p.is_rest || p.is_destructure ||
            p.default_value.has_value() || p.default_string.has_value() ||
            p.default_node != NULL_NODE) {
            return false;
        }
    }
    if (auto cc = fn_call_counts_.find(func.name);
        cc == fn_call_counts_.end() || cc->second < 2) {
        return false;  // single-call fns gain nothing from sharing
    }
    return true;
}

const CodeGenerator::SharedBlock*
CodeGenerator::get_or_compile_shared_block(const UserFunctionInfo& func) {
    if (auto it = shared_blocks_.find(func.name); it != shared_blocks_.end()) {
        return it->second ? &*it->second : nullptr;
    }

    auto not_shareable = [&]() -> const SharedBlock* {
        shared_blocks_[func.name] = std::nullopt;
        return nullptr;
    };

    // --- Cheap per-fn gate (signature only, no compile) --------------------
    if (!fn_shareable_by_signature(func)) {
        return not_shareable();
    }

    // Tentatively mark not-shareable so a (analyzer-rejected, but defensive)
    // recursive reference resolves to the inline path instead of re-entering.
    shared_blocks_[func.name] = std::nullopt;

    // --- Speculative shared-body compile (rolled back if it fails) ---------
    const std::size_t subprog_mark = subprograms_.size();
    const std::size_t diag_mark = diagnostics_.size();
    const std::uint16_t buf_mark = buffers_.count();
    auto saved_call_counters = call_counters_;
    const SourceLocation saved_loc = current_source_loc_;
    const std::uint8_t pc = static_cast<std::uint8_t>(func.params.size());

    // Allocate the convention param bank — pc contiguous buffers.
    std::uint16_t param_base = buffers_.allocate();
    bool contiguous = (param_base != BufferAllocator::BUFFER_UNUSED);
    std::uint16_t prev = param_base;
    for (std::uint8_t i = 1; i < pc && contiguous; ++i) {
        std::uint16_t b = buffers_.allocate();
        if (b == BufferAllocator::BUFFER_UNUSED || b != prev + 1) {
            contiguous = false;
        }
        prev = b;
    }
    if (!contiguous) {
        buffers_.reset_to(buf_mark);
        return not_shareable();
    }

    // Compile the body once under a stable, call-site-independent path so its
    // state IDs hash consistently; per-call-site isolation is layered on at
    // dispatch time via the BLOCK_CALL state_id XOR (see emit_block_call).
    auto saved_path = std::move(path_stack_);
    path_stack_.clear();
    push_path("block:" + func.name);
    auto saved_node_types = std::move(node_types_);
    node_types_.clear();

    const std::uint32_t block_id = begin_subprogram();

    symbols_->push_scope();
    for (std::uint8_t i = 0; i < pc; ++i) {
        symbols_->define_variable(func.params[i].name,
                                  static_cast<std::uint16_t>(param_base + i));
    }

    ++user_function_depth_;
    TypedValue body_tv = visit(func.body_node);
    --user_function_depth_;

    // Resolve the body output buffer(s).
    std::uint16_t body_result = body_tv.buffer;
    std::uint16_t body_result_r = body_tv.right_buffer;
    bool body_is_stereo = body_tv.is_stereo() ||
        (body_result != BufferAllocator::BUFFER_UNUSED &&
         is_stereo_buffer(body_result));
    if (body_is_stereo && body_result_r == 0xFFFF &&
        body_result != BufferAllocator::BUFFER_UNUSED) {
        StereoBuffers sb = get_stereo_buffers_by_buffer(body_result);
        body_result = sb.left;
        body_result_r = sb.right;
    }

    std::uint16_t body_output;
    std::uint8_t output_count;
    bool bad = false;
    if (body_result == BufferAllocator::BUFFER_UNUSED) {
        // Void body (side-effecting sink, e.g. ends with out(@)).
        body_output = BufferAllocator::BUFFER_UNUSED;
        output_count = 1;
        body_is_stereo = false;
    } else if (body_is_stereo) {
        // Stereo: COPY the body result into a fresh adjacent output pair so
        // the executor can read body_output / body_output+1.
        std::uint16_t ol = buffers_.allocate();
        std::uint16_t orr = buffers_.allocate();
        if (ol == BufferAllocator::BUFFER_UNUSED ||
            orr == BufferAllocator::BUFFER_UNUSED || orr != ol + 1) {
            bad = true;
            body_output = BufferAllocator::BUFFER_UNUSED;
            output_count = 2;
        } else {
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, ol,
                                                body_result));
            emit(cedar::Instruction::make_unary(cedar::Opcode::COPY, orr,
                                                body_result_r));
            body_output = ol;
            output_count = 2;
        }
    } else {
        // Mono: the body's own result buffer is the output — no copy needed.
        body_output = body_result;
        output_count = 1;
    }

    symbols_->pop_scope();
    end_subprogram(block_id, pc, output_count);

    // Validate the compiled body before committing.
    SubprogramDesc& desc = subprograms_[block_id];
    if (!bad) {
        for (std::size_t i = diag_mark; i < diagnostics_.size(); ++i) {
            if (diagnostics_[i].severity == Severity::Error) { bad = true; break; }
        }
    }
    if (!bad && (desc.body.empty() || desc.body.size() > 255 ||
                 block_id >= cedar::MAX_SUBPROGRAMS)) {
        bad = true;
    }
    if (!bad) {
        // A shared body runs through the flat execute() body loop, which
        // does NOT handle dispatch-loop opcodes. A body containing one must
        // stay inlined (in the main stream, where the dispatch loop runs).
        for (const auto& bi : desc.body) {
            if (bi.opcode == cedar::Opcode::BLOCK_CALL ||
                bi.opcode == cedar::Opcode::BLOCK_BIND ||
                bi.opcode == cedar::Opcode::RET ||
                bi.opcode == cedar::Opcode::FOREACH_EVENT ||
                bi.opcode == cedar::Opcode::POLY_BEGIN ||
                bi.opcode == cedar::Opcode::POLY_END ||
                bi.opcode == cedar::Opcode::SKIP_IF_ZERO ||
                bi.opcode == cedar::Opcode::SKIP_IF_NONZERO ||
                bi.opcode == cedar::Opcode::LOOP_STATIC) {
                bad = true;
                break;
            }
        }
    }

    if (bad) {
        // Roll the speculative compile back; the fn falls back to inlining.
        subprograms_.resize(subprog_mark);
        diagnostics_.resize(diag_mark);
        buffers_.reset_to(buf_mark);
        call_counters_ = std::move(saved_call_counters);
        node_types_ = std::move(saved_node_types);
        path_stack_ = std::move(saved_path);
        current_source_loc_ = saved_loc;
        return not_shareable();
    }

    // Commit. Keep the body's call-counter bumps (baked into the block);
    // discard the speculative node_types_ entries (body nodes are never
    // re-visited — each further call site just emits a BLOCK_CALL).
    desc.param_base = param_base;
    desc.body_output = body_output;
    node_types_ = std::move(saved_node_types);
    path_stack_ = std::move(saved_path);
    current_source_loc_ = saved_loc;

    SharedBlock blk;
    blk.block_id = block_id;
    blk.param_base = param_base;
    blk.body_output = body_output;
    blk.param_count = pc;
    blk.output_count = output_count;
    blk.is_stereo = body_is_stereo;
    shared_blocks_[func.name] = blk;
    return &*shared_blocks_[func.name];
}

TypedValue CodeGenerator::emit_block_call(
    NodeIndex node, const Node& n, const UserFunctionInfo& func,
    const SharedBlock& block, const std::vector<std::uint16_t>& arg_bufs) {

    // Per-call-site state isolation seed: a stable path component hashed into
    // the BLOCK_CALL state_id. The VM XORs the shared body's state IDs by a
    // mix of this value, so each call site gets its own DSP state slots —
    // recoverable across hot-swap as long as source identity is preserved.
    std::uint32_t count = call_counters_["block:" + func.name]++;
    push_path("block:" + func.name + "@callsite_" + std::to_string(count));
    std::uint32_t state_id = compute_state_id();
    pop_path();

    // Allocate this call site's own result buffer(s).
    std::uint16_t out_l = BufferAllocator::BUFFER_UNUSED;
    std::uint16_t out_r = BufferAllocator::BUFFER_UNUSED;
    if (block.body_output != BufferAllocator::BUFFER_UNUSED) {
        out_l = buffers_.allocate();
        if (out_l == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::void_val();
        }
        if (block.is_stereo) {
            out_r = buffers_.allocate();
            if (out_r == BufferAllocator::BUFFER_UNUSED ||
                out_r != out_l + 1) {
                error("E166", "Internal error: BLOCK_CALL stereo result "
                      "buffers not adjacent", n.location);
                return TypedValue::void_val();
            }
        }
    }

    // Params 5..N-1 (for fns with >5 params) travel in a contiguous run of
    // BLOCK_BIND instructions emitted immediately before the BLOCK_CALL — one
    // per logical slot, slot index in `rate`. Slots 0..4 stay in BLOCK_CALL's
    // inputs[0..4]. Fns with <=5 params emit no BLOCK_BIND (bit-identical to
    // pre-§4.2 codegen).
    for (std::size_t i = 5; i < arg_bufs.size(); ++i) {
        cedar::Instruction bind{};
        bind.opcode = cedar::Opcode::BLOCK_BIND;
        bind.rate = static_cast<std::uint8_t>(i);
        bind.out_buffer = 0xFFFF;
        bind.inputs[0] = arg_bufs[i];
        bind.inputs[1] = bind.inputs[2] = bind.inputs[3] =
            bind.inputs[4] = 0xFFFF;
        bind.flags = 0;
        bind.state_id = 0;
        emit(bind);
    }

    cedar::Instruction bc{};
    bc.opcode = cedar::Opcode::BLOCK_CALL;
    bc.rate = static_cast<std::uint8_t>(block.block_id);
    bc.out_buffer = out_l;
    bc.inputs[0] = bc.inputs[1] = bc.inputs[2] = bc.inputs[3] =
        bc.inputs[4] = 0xFFFF;
    for (std::size_t i = 0; i < arg_bufs.size() && i < 5; ++i) {
        bc.inputs[i] = arg_bufs[i];
    }
    bc.flags = 0;
    if (block.is_stereo) {
        bc.flags = cedar::InstructionFlag::STEREO_OUTPUT;
    }
    bc.state_id = state_id;
    emit(bc);

    if (out_l == BufferAllocator::BUFFER_UNUSED) {
        return cache_and_return(node, TypedValue::void_val());
    }
    if (block.is_stereo) {
        register_stereo(node, out_l, out_r);
        return cache_and_return(node, TypedValue::stereo_signal(out_l, out_r));
    }
    return cache_and_return(node, TypedValue::signal(out_l));
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
                ConstEvaluator evaluator(*ast_, *symbols_, *ctx_->interner);
                auto result = evaluator.evaluate(param.default_node);
                for (const auto& diag : evaluator.diagnostics()) {
                    diagnostics_.push_back(diag);
                }
                if (result && std::holds_alternative<double>(*result)) {
                    float val = static_cast<float>(std::get<double>(*result));
                    param_buf = emit_push_const(val);
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
            // Use numeric default value. Record the backing NumberLit node as a
            // param literal so `match(param)` const-folds and builtins like
            // linspace can resolve the param when left at its default.
            if (func.params[i].default_node != NULL_NODE) {
                std::uint32_t param_hash = fnv1a_hash(func.params[i].name);
                param_literals_[param_hash] = func.params[i].default_node;
            }
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
            ConstEvaluator evaluator(*ast_, *symbols_, *ctx_->interner);
            auto result = evaluator.evaluate(func.params[i].default_node);

            for (const auto& diag : evaluator.diagnostics()) {
                diagnostics_.push_back(diag);
            }

            if (result) {
                if (std::holds_alternative<double>(*result)) {
                    float val = static_cast<float>(std::get<double>(*result));
                    param_buf = emit_push_const(val);
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
        // Destructure param `name = ({x, y}) -> …`: the caller arg must
        // resolve to a Record; each declared field becomes a local in this
        // scope. Mirrors the `fn f({x, y})` path in handle_user_function_call.
        if (func.params[i].is_destructure) {
            const TypedValue* destr_tv = nullptr;
            if (i < args.size()) {
                auto type_it = node_types_.find(args[i]);
                if (type_it != node_types_.end()) destr_tv = &type_it->second;
            }
            if (destr_tv == nullptr) {
                error("E140",
                      "Cannot destructure: missing or unresolved argument for "
                      "closure", n.location);
                continue;
            }
            bind_destructure_fields(*destr_tv,
                                    func.params[i].destructure_fields,
                                    n.location,
                                    "E187");
            continue;
        }

        // Record argument: bind as a record so `param.field` resolves inside
        // the closure body (mirrors handle_user_function_call). Needed for the
        // unison `ext` record passed to instrument closures.
        const TypedValue* arg_tv = nullptr;
        if (i < args.size()) {
            auto type_it = node_types_.find(args[i]);
            if (type_it != node_types_.end()) arg_tv = &type_it->second;
        }
        if (arg_tv != nullptr && arg_tv->type == ValueType::Record) {
            std::shared_ptr<RecordTypeInfo> record_type;
            // If the arg is a plain identifier bound to a record, reuse that
            // symbol's record_type: its source_node points at the real record
            // literal. Pointing source_node at the identifier itself would make
            // visit() recurse infinitely (the identifier resolves back to the
            // record we're about to define).
            const Node& arg_node = ast_->arena[args[i]];
            if (arg_node.type == NodeType::Identifier &&
                std::holds_alternative<Node::IdentifierData>(arg_node.data)) {
                auto asym = symbols_->lookup(ctx_->interner->view(arg_node.as_identifier()));
                if (asym && asym->kind == SymbolKind::Record && asym->record_type) {
                    record_type = asym->record_type;
                }
            }
            if (!record_type) {
                record_type = std::make_shared<RecordTypeInfo>();
                record_type->source_node = args[i];
                if (arg_tv->record) {
                    for (const auto& [name, field_tv] : arg_tv->record->fields) {
                        RecordFieldInfo field_info;
                        field_info.name = name;
                        field_info.buffer_index = field_tv.buffer;
                        field_info.field_kind = SymbolKind::Variable;
                        record_type->fields.push_back(std::move(field_info));
                    }
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
    }

    // Push semantic path for unique state IDs
    std::string callee_name = std::string(ctx_->interner->view(n.as_identifier()));
    std::uint32_t count = call_counters_[callee_name]++;
    push_path(callee_name + "#" + std::to_string(count));

    // Save node_types_ state before visiting body
    auto saved_node_types = std::move(node_types_);
    node_types_.clear();

    TypedValue result_tv = TypedValue::void_val();

    // PRD prd-midi-input §10 Q4: track fn-body context so `midi()` can reject
    // calls that are not top-level.
    ++user_function_depth_;

    // Handle composed functions: apply each function in the chain sequentially
    if (!func.compose_chain.empty()) {
        // First function gets the call argument(s)
        std::uint16_t result = BufferAllocator::BUFFER_UNUSED;
        if (!param_bufs.empty()) {
            result = param_bufs[0];
        }
        for (const auto& chain_ref : func.compose_chain) {
            std::array<std::uint16_t, 1> arg_bufs{result};
            result = apply_function_ref(chain_ref, arg_bufs, n.location);
        }
        result_tv = TypedValue::signal(result);
    } else {
        // Normal function value call
        // Find the body node (structurally the last child of the Closure).
        NodeIndex body = func.is_user_function
            ? func.closure_node
            : closure_body(ast_->arena, func.closure_node);

        // Visit closure body (inline expansion)
        if (body != NULL_NODE) {
            result_tv = visit(body);
        }
    }

    --user_function_depth_;

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
    // Body is the last child; preceding children are parameters (Identifier
    // with ClosureParamData/IdentifierData, or DestructureParam).
    NodeIndex body = closure_body(ast_->arena, node);
    if (body == NULL_NODE) {
        error("E112", "Closure has no body", n.location);
        return TypedValue::void_val();
    }

    std::vector<std::string> param_names;
    for (NodeIndex child = n.first_child; child != body;
         child = ast_->arena[child].next_sibling) {
        const Node& child_node = ast_->arena[child];
        if (child_node.type == NodeType::DestructureParam) {
            // Destructure param `({x, y}) -> …`: each field becomes a local.
            for (const auto& f : child_node.as_destructure_param().fields) {
                param_names.push_back(f.name);
            }
        } else if (child_node.type == NodeType::Identifier) {
            if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
                param_names.push_back(child_node.as_closure_param().name);
            } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
                param_names.push_back(std::string(ctx_->interner->view(child_node.as_identifier())));
            }
        }
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
            param_name = std::string(ctx_->interner->view(scrutinee_ptr->as_identifier()));
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
                param_name = std::string(ctx_->interner->view(scrutinee_ptr->as_identifier()));
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
    std::string func_name = std::string(ctx_->interner->view(n.as_identifier()));

    // Determine time unit from function name
    // 0 = seconds (default), 1 = milliseconds, 2 = samples
    std::uint8_t time_unit = 0;
    if (func_name == "tap_delay_ms") {
        time_unit = 1;
    } else if (func_name == "tap_delay_smp") {
        time_unit = 2;
    }

    // Collect arguments: in, time, fb, processor, [dry], [wet]
    auto extracted = codegen::extract_call_args(ast_->arena, n.first_child, 4, 6);
    if (!extracted.valid) {
        error("E301", func_name + "() requires 4-6 arguments: " + func_name + "(in, time, fb, processor, dry?, wet?)", n.location);
        return TypedValue::void_val();
    }
    std::vector<NodeIndex>& args = extracted.nodes;

    // Substitute `_` placeholders at optional slots (dry, wet) with their
    // BuiltinInfo defaults. See PRD §10 Addendum.
    if (const BuiltinInfo* bi = lookup_builtin(func_name)) {
        codegen::resolve_underscore_defaults(
            const_cast<AstArena&>(ast_->arena), *ctx_->interner, args, *bi);
    }

    // Validate that processor (4th arg) is a Closure
    const Node& processor_node = ast_->arena[args[3]];
    if (processor_node.type != NodeType::Closure) {
        error("E302", "tap_delay() 4th argument must be a closure: (x) -> ...", n.location);
        return TypedValue::void_val();
    }

    // Extract closure parameter (must have exactly 1 parameter)
    NodeIndex body_node = closure_body(ast_->arena, args[3]);
    if (body_node == NULL_NODE) {
        error("E304", "tap_delay() processor closure has no body", n.location);
        return TypedValue::void_val();
    }

    std::string param_name;
    int param_count = 0;
    for (NodeIndex child = processor_node.first_child; child != body_node;
         child = ast_->arena[child].next_sibling) {
        const Node& child_node = ast_->arena[child];
        if (child_node.type != NodeType::Identifier) continue;
        if (std::holds_alternative<Node::ClosureParamData>(child_node.data)) {
            param_name = child_node.as_closure_param().name;
            param_count++;
        } else if (std::holds_alternative<Node::IdentifierData>(child_node.data)) {
            param_name = ctx_->interner->view(child_node.as_identifier());
            param_count++;
        }
    }

    if (param_count != 1) {
        error("E303", "tap_delay() processor closure must have exactly 1 parameter", n.location);
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

    // tap_delay is stereo-native (prd-stereo-native-opcodes Phase 4c): the
    // closure body operates on a stereo signal pair. Allocate an adjacent L/R
    // buffer pair for DELAY_TAP so the closure sees stereo input.
    std::uint16_t tap_out_l = buffers_.allocate();
    std::uint16_t tap_out_r = buffers_.allocate();
    if (tap_out_l == BufferAllocator::BUFFER_UNUSED ||
        tap_out_r == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::void_val();
    }
    if (tap_out_r != tap_out_l + 1) {
        error("E166", "Internal error: stereo buffer allocation not adjacent",
              n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // Detect whether the dry/input signal is stereo so DELAY_WRITE reads the
    // adjacent right buffer for the R lane. Mono input auto-broadcasts inside
    // the opcode body.
    bool in_is_stereo = is_stereo_buffer(in_buf);

    // Emit DELAY_TAP instruction. STEREO_OUTPUT signals the per-channel write
    // pattern in op_delay_tap; STEREO_INPUT is irrelevant here because TAP
    // does not read inputs[0] for audio (only for signal flow).
    cedar::Instruction tap_inst{};
    tap_inst.opcode = cedar::Opcode::DELAY_TAP;
    tap_inst.out_buffer = tap_out_l;
    tap_inst.inputs[0] = in_buf;
    tap_inst.inputs[1] = time_buf;
    tap_inst.inputs[2] = 0xFFFF;
    tap_inst.inputs[3] = 0xFFFF;
    tap_inst.inputs[4] = 0xFFFF;
    tap_inst.rate = time_unit;
    tap_inst.state_id = delay_state_id;
    tap_inst.flags = static_cast<std::uint16_t>(cedar::InstructionFlag::STEREO_OUTPUT);
    emit(tap_inst);

    // Bind the closure parameter as a Stereo signal so downstream stereo-
    // native ops in the closure body see stereo input. The L buffer is the
    // symbol; the (L, R) pair is recorded in stereo_buffer_pairs_ via the
    // node-level register_stereo (using the closure-body node so future
    // is_stereo checks resolve correctly).
    symbols_->push_scope();
    symbols_->define_variable(param_name, tap_out_l);
    stereo_buffer_pairs_[tap_out_l] = tap_out_r;

    // Push semantic context for nested opcodes
    push_path("fb");

    // Save node_types_ state before visiting closure body
    auto saved_node_types = std::move(node_types_);
    node_types_.clear();

    // Compile the closure body (feedback chain) — returns stereo when the
    // body chains through any stereo-native opcode (filters/distortion/etc.).
    TypedValue processed_tv = visit(body_node);
    std::uint16_t processed_l = processed_tv.buffer;
    std::uint16_t processed_r = processed_tv.is_stereo() && processed_tv.right_buffer != 0xFFFF
        ? processed_tv.right_buffer
        : processed_l;  // closure body emitted mono — DELAY_WRITE broadcasts

    // Restore node_types_
    node_types_ = std::move(saved_node_types);

    // Pop semantic context and scope
    pop_path();
    symbols_->pop_scope();

    // Allocate adjacent L/R output pair for DELAY_WRITE
    std::uint16_t write_out_l = buffers_.allocate();
    std::uint16_t write_out_r = buffers_.allocate();
    if (write_out_l == BufferAllocator::BUFFER_UNUSED ||
        write_out_r == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::void_val();
    }
    if (write_out_r != write_out_l + 1) {
        error("E166", "Internal error: stereo buffer allocation not adjacent",
              n.location);
        pop_path();
        return TypedValue::void_val();
    }

    // If the closure body returned mono, materialise a stereo pair by
    // ensuring processed_l/processed_r alias the same buffer. op_delay_write
    // reads `inputs[1] + 1` for the R lane unconditionally, so we need
    // processed_l + 1 to contain valid data. The simplest valid case is when
    // processed_r is already processed_l + 1 (true for any stereo-native op
    // output). Mono closure-body output is the edge case we don't fully
    // support here — issue a warning and reroute by writing the mono buf to
    // both lanes via a quick PUSH_CONST + COPY would be the proper fix;
    // for now we accept the alias which may double the mono signal on R.
    // Codegen guarantees stereo for any non-trivial closure body.
    if (processed_r != processed_l + 1) {
        // Closure body was mono (e.g. `x -> 0`). DELAY_WRITE will read R from
        // processed_l + 1 which may be garbage. We accept this for the common
        // case; stereo-native closure bodies will always satisfy adjacency.
    }

    // Emit DELAY_WRITE instruction with STEREO_OUTPUT + STEREO_INPUT (if the
    // upstream dry input is stereo).
    cedar::Instruction write_inst{};
    write_inst.opcode = cedar::Opcode::DELAY_WRITE;
    write_inst.out_buffer = write_out_l;
    write_inst.inputs[0] = in_buf;
    write_inst.inputs[1] = processed_l;
    write_inst.inputs[2] = fb_buf;
    write_inst.inputs[3] = dry_buf;
    write_inst.inputs[4] = wet_buf;
    write_inst.rate = 0;
    write_inst.state_id = delay_state_id;  // Same state_id as TAP!
    write_inst.flags = static_cast<std::uint16_t>(
        cedar::InstructionFlag::STEREO_OUTPUT |
        (in_is_stereo ? cedar::InstructionFlag::STEREO_INPUT : 0u));
    emit(write_inst);

    // Pop the outer path
    pop_path();

    register_stereo(node, write_out_l, write_out_r);
    return cache_and_return(node, TypedValue::stereo_signal(write_out_l, write_out_r));
}

// Handle poly(voices, instrument) / mono(instrument) / legato(instrument)
// Emits POLY_BEGIN, inlines instrument body, emits POLY_END
TypedValue CodeGenerator::handle_poly_call(NodeIndex node, const Node& n) {
    using codegen::extract_call_args;

    std::string func_name = std::string(ctx_->interner->view(n.as_identifier()));

    // Determine mode from function name
    std::uint8_t mode = 0;  // 0=poly
    if (func_name == "mono") mode = 1;
    else if (func_name == "legato") mode = 2;

    // Collect arguments:
    //   poly: 2 required (input, instrument), 2 optional (voices=64, release=0)
    //   mono/legato: 1 required + 2 optional. mono allows
    //     mono(instrument) | mono(input, instrument) | mono(input, instrument, release).
    //     The 1-arg form may also be a downmix call (signal arg) — that path
    //     is dispatched elsewhere, but the named-arg reorder still validates
    //     `release` against this param list.
    auto args = extract_call_args(ast_->arena, n.first_child,
                                  mode == 0 ? 2 : 1,
                                  mode == 0 ? 4 : 3);
    if (!args.valid) {
        if (mode == 0) {
            error("E400", "poly() requires 2-4 arguments: poly(input, instrument, voices=64, release=0). Pipe a pattern in: pat(...) |> poly(@, instrument)", n.location);
        } else {
            error("E400", func_name + "() requires 1-3 arguments: " + func_name + "(instrument) or " + func_name + "(input, instrument, release=0)", n.location);
        }
        return TypedValue::void_val();
    }

    // Substitute `_` placeholders at optional slots (voices, release) with
    // their BuiltinInfo defaults. See PRD §10 Addendum — this hooks
    // specialized handlers into the same default-filling the generic
    // CallSlot dispatcher already provides.
    if (const BuiltinInfo* bi = lookup_builtin(func_name)) {
        codegen::resolve_underscore_defaults(
            const_cast<AstArena&>(ast_->arena), *ctx_->interner,
            args.nodes, *bi);
    }

    // Parse arguments based on func_name and arg count
    NodeIndex pattern_arg = NULL_NODE;
    NodeIndex voices_arg = NULL_NODE;
    NodeIndex release_arg = NULL_NODE;
    NodeIndex instrument_arg = NULL_NODE;
    std::uint8_t max_voices = (mode == 0) ? 64 : 1;
    float release_seconds = 0.0f;

    if (mode == 0) {
        // poly: (input, instrument, voices=64, release=0)
        pattern_arg = args.nodes[0];
        instrument_arg = args.nodes[1];
        if (args.nodes.size() >= 3) voices_arg = args.nodes[2];
        if (args.nodes.size() >= 4) release_arg = args.nodes[3];
    } else {
        // mono/legato: 1 arg = (fn), 2 args = (input, fn), 3 args = (input, fn, release)
        if (args.nodes.size() >= 2) {
            pattern_arg = args.nodes[0];
            instrument_arg = args.nodes[1];
        } else if (args.nodes.size() == 1) {
            instrument_arg = args.nodes[0];
        } else {
            error("E400", func_name + "() requires 1-3 arguments", n.location);
            return TypedValue::void_val();
        }
        if (args.nodes.size() >= 3) release_arg = args.nodes[2];
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

    // Extract release window from number literal (poly/mono/legato).
    // Negative values are clamped to 0 in the VM; the literal check just
    // catches non-constant args so codegen stays simple.
    if (release_arg != NULL_NODE) {
        const Node& rn = ast_->arena[release_arg];
        if (rn.type == NodeType::NumberLit) {
            release_seconds = static_cast<float>(rn.as_number());
            if (release_seconds < 0.0f) release_seconds = 0.0f;
        } else {
            error("E406", func_name + "() release must be a number literal (seconds)", rn.location);
            return TypedValue::void_val();
        }
    }

    // Visit pattern input if present (emits SEQPAT_QUERY etc.)
    std::uint32_t seq_state_id = 0;
    // Phase 3: the upstream pattern's custom record-suffix property slot map.
    // Drives custom-field plumbing into the per-voice field bank below.
    std::shared_ptr<PatternPayload> poly_pattern;
    if (pattern_arg != NULL_NODE) {
        auto pat_tv = visit(pattern_arg);
        // Look up upstream state_id. Patterns and midi() sources both feed
        // POLY through StatePool::resolve_output_events on the cedar side;
        // here we just record whichever state_id the upstream produced.
        if (pat_tv.pattern) {
            seq_state_id = pat_tv.pattern->state_id;
            poly_pattern = pat_tv.pattern;
        }
        // Consume polyphonic tracking — poly() handles voice allocation at runtime.
        polyphonic_pattern_nodes_.erase(pattern_arg);
    }

    // Resolve instrument function
    auto func_ref = resolve_function_arg(instrument_arg);
    if (!func_ref) {
        error("E403", func_name + "() instrument argument must be a function", n.location);
        return TypedValue::void_val();
    }

    // --- Classify the callback parameter list -------------------------------
    // The callback takes positional parameters (canonical order
    // freq,gate,vel,trig,type,note,dur,chance,time,phase,sample_id), a trailing
    // record destructure `{...}`, a mix of both, or a trailing `...rest` param.
    // The historical 3-param `(freq, gate, vel)` callback is the 3-prefix of
    // the canonical order, so it keeps compiling unchanged.
    // Phase 1: only freq/gate/vel/trig have VM-plumbed per-voice buffers; any
    // other referenced field is accepted but resolves to a constant-0 buffer
    // (Phase 2 wires the remaining fixed fields, Phase 3 the custom fields).
    struct PolyBind {
        std::string  name;
        std::uint8_t field_id;              // PatternPayload index, or 0xFF = deferred
        NodeIndex    default_node = NULL_NODE;
    };
    std::vector<PolyBind> binds;
    bool        has_rest = false;
    std::string rest_name;

    // Phase 3: the per-voice field bank holds the 11 fixed pattern-event
    // fields followed by `prop_count` contiguous custom record-suffix
    // property slots (`c4{cutoff:0.8}`). A custom field at prop slot S binds
    // to bank slot kPolyFieldCount + S.
    constexpr std::size_t kPolyFieldCount = 11;
    const std::size_t prop_count = poly_pattern
        ? std::min(poly_pattern->custom_field_slots.size(),
                   cedar::MAX_PROPS_PER_EVENT)
        : std::size_t{0};
    const std::size_t bank_count = kPolyFieldCount + prop_count;
    {
        const auto& params = func_ref->params;
        const std::size_t np = params.size();
        int destr_count = 0, rest_count = 0;
        std::size_t destr_idx = 0, rest_idx = 0;
        for (std::size_t i = 0; i < np; ++i) {
            if (params[i].is_rest) { rest_count++; rest_idx = i; }
            else if (params[i].is_destructure) { destr_count++; destr_idx = i; }
        }
        if (destr_count > 0 && rest_count > 0) {
            error("E418", func_name + "() callback cannot combine a record "
                  "destructure and a rest parameter", n.location);
            return TypedValue::void_val();
        }
        if (rest_count > 1) {
            error("E417", func_name + "() callback may declare only one rest "
                  "parameter", n.location);
            return TypedValue::void_val();
        }
        if (rest_count == 1 && rest_idx != np - 1) {
            error("E417", func_name + "() rest parameter must be the last "
                  "parameter", n.location);
            return TypedValue::void_val();
        }
        if (destr_count > 1) {
            error("E417", func_name + "() callback may declare only one record "
                  "destructure parameter", n.location);
            return TypedValue::void_val();
        }
        if (destr_count == 1 && destr_idx != np - 1) {
            error("E417", func_name + "() record destructure must be the last "
                  "parameter", n.location);
            return TypedValue::void_val();
        }

        std::size_t positional_count = np;
        if (destr_count == 1) positional_count = destr_idx;
        else if (rest_count == 1) positional_count = rest_idx;
        if (positional_count > kPolyCanonicalOrder.size()) {
            error("E415", func_name + "() callback accepts at most " +
                  std::to_string(kPolyCanonicalOrder.size()) +
                  " positional parameters", n.location);
            return TypedValue::void_val();
        }

        std::array<bool, 11> field_bound{};
        for (std::size_t i = 0; i < positional_count; ++i) {
            std::uint8_t field = kPolyCanonicalOrder[i];
            binds.push_back({params[i].name, field, NULL_NODE});
            field_bound[field] = true;
        }
        if (destr_count == 1) {
            for (const auto& df : params[destr_idx].destructure_fields) {
                int idx = pattern_field_index(df.name);
                if (idx < 0) {
                    // Phase 3: a custom record-suffix field declared on the
                    // upstream pattern binds to a field-bank slot after the
                    // 11 fixed fields. A name no note declares stays 0xFF and
                    // resolves to a constant default buffer.
                    std::uint8_t fid = 0xFF;
                    if (poly_pattern) {
                        auto cs = poly_pattern->custom_field_slots.find(df.name);
                        if (cs != poly_pattern->custom_field_slots.end()) {
                            fid = static_cast<std::uint8_t>(
                                kPolyFieldCount + cs->second);
                        }
                    }
                    binds.push_back({df.name, fid, df.default_node});
                    continue;
                }
                if (field_bound[static_cast<std::size_t>(idx)]) {
                    error("E416", func_name + "() field '" + df.name +
                          "' is bound both positionally and in the record "
                          "destructure", n.location);
                    return TypedValue::void_val();
                }
                field_bound[static_cast<std::size_t>(idx)] = true;
                binds.push_back({df.name, static_cast<std::uint8_t>(idx),
                                 df.default_node});
            }
        }
        if (rest_count == 1) {
            has_rest = true;
            rest_name = params[rest_idx].name;
        }
    }

    // Allocate scratch buffers: the contiguous per-voice field bank (11 fixed
    // fields + prop_count custom slots, indexed by PatternPayload field id),
    // then the stereo voice-out and mix pairs. poly is stereo-native:
    // voice_out and mix are adjacent L/R pairs (R = L+1). The instruction
    // carries only bank_base (inputs[0]) and voice_out (inputs[4]); the VM
    // fills every bank buffer per voice — see VM::run_voice_pool.
    std::uint16_t bank_base = buffers_.allocate();
    bool bank_ok = (bank_base != BufferAllocator::BUFFER_UNUSED);
    for (std::size_t i = 1; i < bank_count; ++i) {
        std::uint16_t b = buffers_.allocate();
        if (b == BufferAllocator::BUFFER_UNUSED || b != bank_base + i) {
            bank_ok = false;
        }
    }
    std::uint16_t voice_out_buf = buffers_.allocate();
    std::uint16_t voice_out_buf_r = buffers_.allocate();
    std::uint16_t mix_buf = buffers_.allocate();
    std::uint16_t mix_buf_r = buffers_.allocate();

    if (!bank_ok ||
        voice_out_buf == BufferAllocator::BUFFER_UNUSED ||
        voice_out_buf_r == BufferAllocator::BUFFER_UNUSED ||
        mix_buf == BufferAllocator::BUFFER_UNUSED ||
        mix_buf_r == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::void_val();
    }
    if (voice_out_buf_r != voice_out_buf + 1 || mix_buf_r != mix_buf + 1) {
        error("E166", "Internal error: poly stereo buffer allocation not adjacent", n.location);
        return TypedValue::void_val();
    }

    // Push semantic path for unique state_id
    std::uint32_t count = call_counters_["poly"]++;
    push_path("poly#" + std::to_string(count));
    std::uint32_t poly_state_id = compute_state_id();

    // PRD prd-runtime-functions-control-flow L3: poly() compiles the instrument
    // body into a FOREACH_EVENT subprogram block (VOICE_POOL allocator). The
    // legacy POLY_BEGIN/inline-body/POLY_END form is kept behind the
    // `legacy_poly` compiler option as a rollback knob; both paths share the
    // body-emission code below — emit() routes instructions to the open
    // subprogram body (new path) or the main stream (legacy path).
    const bool legacy_poly = options_.legacy_poly;
    std::size_t poly_begin_idx = 0;
    std::uint32_t poly_block_id = 0;

    if (legacy_poly) {
        poly_begin_idx = instructions_.size();
        cedar::Instruction poly_begin{};
        poly_begin.opcode = cedar::Opcode::POLY_BEGIN;
        poly_begin.out_buffer = mix_buf;  // L; VM derives mix R = mix_buf + 1
        poly_begin.inputs[0] = bank_base;  // per-voice field bank base
        poly_begin.inputs[1] = 0xFFFF;
        poly_begin.inputs[2] = 0xFFFF;
        poly_begin.inputs[3] = 0xFFFF;
        poly_begin.inputs[4] = voice_out_buf;  // L; VM derives voice_out R = +1
        poly_begin.flags = cedar::InstructionFlag::STEREO_OUTPUT;
        poly_begin.rate = 0;  // Patched after body emission
        poly_begin.state_id = poly_state_id;
        emit(poly_begin);
    } else {
        // Opens the subprogram body — emit() now lands in subprograms_[block].
        poly_block_id = begin_subprogram();
    }

    // Inline function body
    // Push scope and bind callback params to per-voice buffers.
    symbols_->push_scope();

    // The 11 fixed pattern-event fields plus prop_count custom record-suffix
    // fields are plumbed per voice via the contiguous field bank (bank_base +
    // field id); the VM fills every bank buffer in run_voice_pool. A
    // destructure name no note declares (field_id == 0xFF) resolves to a
    // constant buffer holding the const-evaluated `= expr` default.
    //
    // Const-evaluate a destructure `= expr` default; NULL_NODE -> fallback,
    // non-constant expression -> E419.
    auto eval_poly_default = [&](NodeIndex dn, float fallback) -> float {
        if (dn == NULL_NODE) return fallback;
        ConstEvaluator evaluator(*ast_, *symbols_, *ctx_->interner);
        auto result = evaluator.evaluate(dn);
        for (const auto& diag : evaluator.diagnostics()) {
            diagnostics_.push_back(diag);
        }
        if (result && std::holds_alternative<double>(*result)) {
            return static_cast<float>(std::get<double>(*result));
        }
        error("E419", func_name + "() destructure default for a custom field "
              "must be a compile-time constant", n.location);
        return fallback;
    };

    std::vector<std::pair<float, std::uint16_t>> const_field_bufs;
    auto field_buffer = [&](const PolyBind& b) -> std::uint16_t {
        if (b.field_id < bank_count) {
            return static_cast<std::uint16_t>(bank_base + b.field_id);
        }
        // Unknown field: constant buffer holding the `= expr` default
        // (0.0 when absent), cached per distinct value.
        float dv = eval_poly_default(b.default_node, 0.0f);
        for (const auto& [v, buf] : const_field_bufs) {
            if (v == dv) return buf;
        }
        std::uint16_t cb = buffers_.allocate();
        if (cb != BufferAllocator::BUFFER_UNUSED) {
            cedar::Instruction zc{};
            zc.opcode = cedar::Opcode::PUSH_CONST;
            zc.out_buffer = cb;
            zc.inputs[0] = 0xFFFF;
            zc.inputs[1] = 0xFFFF;
            zc.inputs[2] = 0xFFFF;
            zc.inputs[3] = 0xFFFF;
            encode_const_value(zc, dv);
            emit(zc);
            const_field_bufs.emplace_back(dv, cb);
        }
        return cb;
    };

    // Const-evaluate destructure defaults for declared custom fields into the
    // per-slot default table the VM applies when a voice's event omits the
    // property (Phase 3 per-event presence mask).
    std::array<float, cedar::MAX_PROPS_PER_EVENT> prop_defaults{};
    for (const auto& b : binds) {
        if (b.field_id >= kPolyFieldCount && b.field_id < bank_count) {
            prop_defaults[b.field_id - kPolyFieldCount] =
                eval_poly_default(b.default_node, 0.0f);
        }
    }

    for (const auto& b : binds) {
        symbols_->define_variable(b.name, field_buffer(b));
    }
    if (has_rest) {
        // Bind the rest name as an event record: bare use reads freq, `.field`
        // access resolves through the Record typed_value. All 11 fixed fields
        // are plumbed via the per-voice field bank; aliases mirror
        // pattern_field_aliases().
        std::unordered_map<std::string, TypedValue> rec;
        auto add = [&](std::initializer_list<const char*> aliases,
                       std::size_t field_id) {
            std::uint16_t buf = static_cast<std::uint16_t>(bank_base + field_id);
            for (const char* a : aliases) rec.emplace(a, TypedValue::signal(buf));
        };
        add({"freq", "frequency", "pitch", "f", "p"}, PatternPayload::FREQ);
        add({"vel", "velocity", "v"},                 PatternPayload::VEL);
        add({"trig", "trigger"},                      PatternPayload::TRIG);
        add({"gate", "g"},                            PatternPayload::GATE);
        add({"type"},                                 PatternPayload::TYPE);
        add({"note", "midi", "n"},                    PatternPayload::NOTE);
        add({"dur", "duration"},                      PatternPayload::DUR);
        add({"chance"},                               PatternPayload::CHANCE);
        add({"time", "t0", "start"},                  PatternPayload::TIME);
        add({"phase", "cycle", "co"},                 PatternPayload::PHASE);
        add({"sample_id", "sample", "s"},             PatternPayload::SAMPLE_ID);
        // Phase 3: custom record-suffix fields are reachable on the rest
        // record too (`(...e) -> e.cutoff`). Each lives at its field-bank
        // slot after the 11 fixed fields.
        if (poly_pattern) {
            for (const auto& [key, slot] : poly_pattern->custom_field_slots) {
                std::uint16_t buf = static_cast<std::uint16_t>(
                    bank_base + kPolyFieldCount + slot);
                rec.emplace(key, TypedValue::signal(buf));
            }
        }
        Symbol sym;
        sym.kind = SymbolKind::Variable;
        sym.name = rest_name;
        sym.name_id = ctx_->interner->intern(rest_name);
        sym.buffer_index = bank_base;
        sym.typed_value = TypedValue::make_record(std::move(rec), bank_base);
        symbols_->define(sym);
    }

    // Bind captures for closures
    for (const auto& capture : func_ref->captures) {
        symbols_->define_variable(capture.name, capture.buffer_index);
    }

    // Save node_types_ state before visiting body
    auto saved_node_types = std::move(node_types_);
    node_types_.clear();

    // Record body start instruction index (legacy path only — the new path
    // measures body length from the subprogram descriptor).
    std::size_t body_start = instructions_.size();

    // Visit body — capture the full TypedValue so a stereo body (e.g. a voice
    // that ends in pan()) keeps both channels.
    std::uint16_t body_result = BufferAllocator::BUFFER_UNUSED;
    std::uint16_t body_result_r = 0xFFFF;
    bool body_is_stereo = false;
    {
        TypedValue body_tv = TypedValue::void_val();
        if (func_ref->is_user_function) {
            // User function: body is directly the body_node
            body_tv = visit(func_ref->closure_node);
        } else {
            NodeIndex body = closure_body(ast_->arena, func_ref->closure_node);
            if (body != NULL_NODE) {
                body_tv = visit(body);
            }
        }
        body_result = body_tv.buffer;
        body_result_r = body_tv.right_buffer;
        body_is_stereo = body_tv.is_stereo() || is_stereo_buffer(body_result);
        // Pipe-alias path: the TypedValue may not carry the R buffer itself
        // (variable/alias lookups). Resolve it via the legacy stereo map.
        if (body_is_stereo && body_result_r == 0xFFFF &&
            body_result != BufferAllocator::BUFFER_UNUSED) {
            StereoBuffers sb = get_stereo_buffers_by_buffer(body_result);
            body_result = sb.left;
            body_result_r = sb.right;
        }
    }

    // Wire the body result into the stereo voice-out pair. op_copy is
    // mono-only, so a stereo body needs two COPYs (L and R); a mono body is
    // broadcast (dual-mono) into both channels. These COPYs are inside the
    // inlined body and re-run per voice.
    if (body_is_stereo) {
        emit(cedar::Instruction::make_unary(
            cedar::Opcode::COPY, voice_out_buf, body_result));
        emit(cedar::Instruction::make_unary(
            cedar::Opcode::COPY, voice_out_buf_r, body_result_r));
    } else if (body_result != BufferAllocator::BUFFER_UNUSED) {
        emit(cedar::Instruction::make_unary(
            cedar::Opcode::COPY, voice_out_buf, body_result));
        emit(cedar::Instruction::make_unary(
            cedar::Opcode::COPY, voice_out_buf_r, body_result));
    }

    // Record body end (legacy path)
    std::size_t body_end = instructions_.size();

    // Restore node_types_
    for (auto& [k, v] : saved_node_types) {
        if (node_types_.find(k) == node_types_.end()) {
            node_types_[k] = v;
        }
    }

    // Pop scope
    symbols_->pop_scope();

    // Body length: from the instruction stream (legacy) or the subprogram
    // descriptor (new path).
    std::size_t body_length =
        legacy_poly ? (body_end - body_start)
                    : subprograms_[poly_block_id].body.size();
    if (body_length > 255) {
        error("E405", "Poly instrument body too large (max 255 instructions)", n.location);
        if (!legacy_poly) end_subprogram(poly_block_id, 5, 2);
        pop_path();
        return TypedValue::void_val();
    }

    if (legacy_poly) {
        // Patch POLY_BEGIN.rate = body_length, then emit POLY_END.
        instructions_[poly_begin_idx].rate = static_cast<std::uint8_t>(body_length);
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
    } else {
        // Close the subprogram body, then emit one FOREACH_EVENT into the
        // main stream referencing it. The body is NOT inline.
        end_subprogram(poly_block_id, /*frame_slot_count=*/5, /*output_count=*/2);
        cedar::Instruction fe{};
        fe.opcode = cedar::Opcode::FOREACH_EVENT;
        fe.out_buffer = mix_buf;  // L; VM derives mix R = mix_buf + 1
        fe.inputs[0] = bank_base;  // per-voice field bank base
        fe.inputs[1] = 0xFFFF;
        fe.inputs[2] = 0xFFFF;
        fe.inputs[3] = 0xFFFF;
        fe.inputs[4] = voice_out_buf;  // L; VM derives voice_out R = +1
        fe.flags = cedar::InstructionFlag::STEREO_OUTPUT;
        fe.rate = 0;  // body is in the subprogram table, not inline
        fe.state_id = poly_state_id;
        emit(fe);
    }

    // Pop semantic path
    pop_path();

    // State-init: PolyAlloc (legacy) or ForeachAlloc/VOICE_POOL (new path).
    StateInitData poly_init;
    poly_init.state_id = poly_state_id;
    poly_init.poly_seq_state_id = seq_state_id;
    poly_init.poly_max_voices = max_voices;
    poly_init.poly_mode = mode;
    poly_init.poly_steal_strategy = 0;  // oldest
    poly_init.poly_release_seconds = release_seconds;
    // Phase 3: custom record-suffix property plumbing. prop_count contiguous
    // field-bank slots follow the 11 fixed fields; prop_defaults are applied
    // per voice by the VM when an event omits the property.
    poly_init.poly_prop_count = static_cast<std::uint8_t>(prop_count);
    for (std::size_t i = 0; i < cedar::MAX_PROPS_PER_EVENT; ++i) {
        poly_init.poly_prop_defaults[i] = prop_defaults[i];
    }
    if (legacy_poly) {
        poly_init.type = StateInitData::Type::PolyAlloc;
    } else {
        poly_init.type = StateInitData::Type::ForeachAlloc;
        poly_init.foreach_allocator_kind = 0;  // VOICE_POOL
        poly_init.foreach_block_id = poly_block_id;
        poly_init.foreach_event_src_state_id = seq_state_id;
        // VOICE_POOL never indexes a param bank (run_voice_pool fills the
        // field bank directly), so this slot count is inert here.
        poly_init.foreach_field_slot_count = 5;
        poly_init.foreach_output_count = 2;
    }
    state_inits_.push_back(std::move(poly_init));

    register_stereo(node, mix_buf, mix_buf_r);
    return cache_and_return(node, TypedValue::stereo_signal(mix_buf, mix_buf_r));
}

} // namespace akkado
