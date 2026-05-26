#include "akkado/const_eval.hpp"
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <numeric>

namespace akkado {

ConstEvaluator::ConstEvaluator(const Ast& ast, const SymbolTable& symbols,
                               const StringInterner& interner)
    : ast_(&ast), symbols_(&symbols), interner_(&interner) {}

std::optional<ConstValue> ConstEvaluator::evaluate(NodeIndex node) {
    bindings_.clear();
    depth_ = 0;
    return eval(node);
}

std::optional<ConstValue> ConstEvaluator::evaluate_with_bindings(
    NodeIndex node,
    const std::unordered_map<std::string, ConstValue>& bindings) {
    bindings_ = bindings;
    depth_ = 0;
    return eval(node);
}

std::optional<ConstValue> ConstEvaluator::eval(NodeIndex node) {
    if (node == NULL_NODE) return std::nullopt;

    const Node& n = ast_->arena[node];

    switch (n.type) {
        case NodeType::NumberLit:
            return ConstValue{n.as_number()};

        case NodeType::BoolLit:
            return ConstValue{n.as_bool() ? 1.0 : 0.0};

        case NodeType::PitchLit: {
            // MIDI note -> Hz: 440 * 2^((note - 69) / 12)
            double midi = static_cast<double>(n.as_pitch());
            double hz = 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
            return ConstValue{hz};
        }

        case NodeType::Identifier: {
            std::string name;
            if (std::holds_alternative<Node::IdentifierData>(n.data)) {
                name = std::string(interner_->view(n.as_identifier()));
            } else if (std::holds_alternative<Node::ClosureParamData>(n.data)) {
                name = n.as_closure_param().name;
            }
            if (name.empty()) return std::nullopt;

            // Check bindings first (function parameters)
            auto bind_it = bindings_.find(name);
            if (bind_it != bindings_.end()) {
                return bind_it->second;
            }

            // Check symbol table for const values
            auto sym = symbols_->lookup(name);
            if (sym && sym->is_const && sym->const_value.has_value()) {
                return *sym->const_value;
            }

            error("E203", "Non-const identifier '" + name + "' in const expression", n.location);
            return std::nullopt;
        }

        case NodeType::Call:
            return eval_call(node, n);

        case NodeType::ArrayLit: {
            std::vector<double> elements;
            NodeIndex child = n.first_child;
            while (child != NULL_NODE) {
                auto val = eval(child);
                if (!val) return std::nullopt;

                auto scalar = as_scalar(*val, ast_->arena[child].location);
                if (!scalar) return std::nullopt;
                elements.push_back(*scalar);

                child = ast_->arena[child].next_sibling;
            }
            return ConstValue{std::move(elements)};
        }

        case NodeType::Block: {
            // Evaluate statements sequentially, return last value
            std::optional<ConstValue> result;
            NodeIndex child = n.first_child;
            while (child != NULL_NODE) {
                result = eval(child);
                if (!result && has_errors()) return std::nullopt;
                child = ast_->arena[child].next_sibling;
            }
            return result;
        }

        case NodeType::ConstDecl: {
            // const x = expr inside a block
            std::string name = std::string(interner_->view(n.as_identifier()));
            NodeIndex rhs = n.first_child;
            if (rhs == NULL_NODE) return std::nullopt;
            auto val = eval(rhs);
            if (!val) return std::nullopt;
            bindings_[name] = *val;
            return val;
        }

        case NodeType::Assignment: {
            // let x = expr inside a const fn block
            std::string name = std::string(interner_->view(n.as_identifier()));
            NodeIndex rhs = n.first_child;
            if (rhs == NULL_NODE) return std::nullopt;
            auto val = eval(rhs);
            if (!val) return std::nullopt;
            bindings_[name] = *val;
            return val;
        }

        case NodeType::Argument: {
            // Unwrap argument wrapper
            if (n.first_child != NULL_NODE) {
                return eval(n.first_child);
            }
            return std::nullopt;
        }

        case NodeType::MatchExpr: {
            // Evaluate match expression at compile time
            bool has_scrutinee = false;
            if (std::holds_alternative<Node::MatchExprData>(n.data)) {
                has_scrutinee = n.as_match_expr().has_scrutinee;
            }

            std::optional<ConstValue> scrutinee_val;
            NodeIndex first_arm = n.first_child;

            if (has_scrutinee && first_arm != NULL_NODE) {
                scrutinee_val = eval(first_arm);
                if (!scrutinee_val) return std::nullopt;
                first_arm = ast_->arena[first_arm].next_sibling;
            }

            // Iterate arms
            NodeIndex arm = first_arm;
            while (arm != NULL_NODE) {
                const Node& arm_node = ast_->arena[arm];
                if (arm_node.type == NodeType::MatchArm) {
                    const auto& arm_data = arm_node.as_match_arm();

                    if (arm_data.is_wildcard) {
                        // Wildcard matches everything
                        NodeIndex pattern = arm_node.first_child;
                        NodeIndex body = (pattern != NULL_NODE) ?
                            ast_->arena[pattern].next_sibling : NULL_NODE;
                        if (body != NULL_NODE) return eval(body);
                        return std::nullopt;
                    }

                    if (arm_data.is_range && scrutinee_val) {
                        auto sv = as_scalar(*scrutinee_val, n.location);
                        if (sv && *sv >= arm_data.range_low && *sv < arm_data.range_high) {
                            NodeIndex pattern = arm_node.first_child;
                            NodeIndex body = (pattern != NULL_NODE) ?
                                ast_->arena[pattern].next_sibling : NULL_NODE;
                            if (body != NULL_NODE) return eval(body);
                        }
                    } else if (scrutinee_val) {
                        // Compare pattern with scrutinee
                        NodeIndex pattern = arm_node.first_child;
                        if (pattern != NULL_NODE) {
                            auto pat_val = eval(pattern);
                            if (pat_val) {
                                auto sv = as_scalar(*scrutinee_val, n.location);
                                auto pv = as_scalar(*pat_val, ast_->arena[pattern].location);
                                if (sv && pv && *sv == *pv) {
                                    NodeIndex body = ast_->arena[pattern].next_sibling;
                                    if (body != NULL_NODE) return eval(body);
                                }
                            }
                        }
                    } else if (arm_data.has_guard) {
                        // Guard-only match: evaluate guard
                        if (arm_data.guard_node != NULL_NODE) {
                            auto guard_val = eval(arm_data.guard_node);
                            if (guard_val) {
                                auto gv = as_scalar(*guard_val, n.location);
                                if (gv && *gv != 0.0) {
                                    NodeIndex pattern = arm_node.first_child;
                                    NodeIndex body = (pattern != NULL_NODE) ?
                                        ast_->arena[pattern].next_sibling : NULL_NODE;
                                    if (body != NULL_NODE) return eval(body);
                                }
                            }
                        }
                    }
                }
                arm = ast_->arena[arm].next_sibling;
            }

            // No arm matched
            return ConstValue{0.0};
        }

        default:
            error("E202", std::string("Cannot evaluate ") + node_type_name(n.type) +
                  " at compile time", n.location);
            return std::nullopt;
    }
}

std::optional<ConstValue> ConstEvaluator::eval_call(NodeIndex node, const Node& n) {
    std::string func_name = std::string(interner_->view(n.as_identifier()));

    // Collect arguments
    std::vector<ConstValue> args;
    std::vector<NodeIndex> arg_nodes;
    NodeIndex child = n.first_child;
    while (child != NULL_NODE) {
        NodeIndex arg_node = child;
        const Node& cn = ast_->arena[child];
        if (cn.type == NodeType::Argument) {
            arg_node = cn.first_child;
        }
        if (arg_node != NULL_NODE) {
            // Check if argument is a Closure (for map/fold) - skip evaluation
            const Node& arg_n = ast_->arena[arg_node];
            if (arg_n.type == NodeType::Closure) {
                // Closures handled specially by map/fold
                arg_nodes.push_back(arg_node);
                args.push_back(ConstValue{0.0});  // Placeholder
            } else {
                auto val = eval(arg_node);
                if (!val) return std::nullopt;
                arg_nodes.push_back(arg_node);
                args.push_back(std::move(*val));
            }
        }
        child = ast_->arena[child].next_sibling;
    }

    // Check for user-defined const fn
    auto sym = symbols_->lookup(func_name);
    if (sym && sym->kind == SymbolKind::UserFunction && sym->user_function.is_const) {
        return eval_const_fn_call(sym->user_function, args, n.location);
    }

    // Handle array operations
    // step is the interval size (always treated as positive); direction is
    // determined by start vs end (start > end auto-reverses).
    if (func_name == "range") {
        if (args.size() < 2 || args.size() > 3) {
            error("E153", "range() requires 2 or 3 arguments", n.location);
            return std::nullopt;
        }
        auto start = as_scalar(args[0], n.location);
        auto end = as_scalar(args[1], n.location);
        if (!start || !end) return std::nullopt;
        double step_d = 1.0;
        if (args.size() == 3) {
            auto step_opt = as_scalar(args[2], n.location);
            if (!step_opt) return std::nullopt;
            step_d = *step_opt;
        }

        int s = static_cast<int>(*start);
        int e = static_cast<int>(*end);
        int step_mag = std::abs(static_cast<int>(step_d));
        if (step_mag == 0) {
            error("E153", "range() step must be non-zero", n.location);
            return std::nullopt;
        }
        int step = (s <= e) ? step_mag : -step_mag;

        std::vector<double> result;
        auto in_range = [&](int i) { return step > 0 ? i < e : i > e; };
        for (int i = s; in_range(i); i += step) {
            result.push_back(static_cast<double>(i));
        }
        return ConstValue{std::move(result)};
    }

    if (func_name == "map") {
        if (args.size() != 2) {
            error("E152", "map() requires 2 arguments", n.location);
            return std::nullopt;
        }
        auto arr = as_array(args[0]);
        NodeIndex closure_node = arg_nodes[1];
        const Node& closure = ast_->arena[closure_node];
        if (closure.type != NodeType::Closure) {
            error("E202", "map() second argument must be a closure", n.location);
            return std::nullopt;
        }

        // Extract closure parameter name
        std::string param_name;
        NodeIndex body = NULL_NODE;
        NodeIndex cchild = closure.first_child;
        while (cchild != NULL_NODE) {
            const Node& cc = ast_->arena[cchild];
            if (cc.type == NodeType::Identifier) {
                if (std::holds_alternative<Node::ClosureParamData>(cc.data)) {
                    param_name = cc.as_closure_param().name;
                } else if (std::holds_alternative<Node::IdentifierData>(cc.data)) {
                    param_name = std::string(interner_->view(cc.as_identifier()));
                } else {
                    body = cchild;
                    break;
                }
            } else {
                body = cchild;
                break;
            }
            cchild = ast_->arena[cchild].next_sibling;
        }

        if (param_name.empty() || body == NULL_NODE) {
            error("E202", "Invalid closure in map()", n.location);
            return std::nullopt;
        }

        std::vector<double> result;
        auto saved_bindings = bindings_;
        for (double elem : arr) {
            bindings_ = saved_bindings;
            bindings_[param_name] = ConstValue{elem};
            auto val = eval(body);
            if (!val) return std::nullopt;
            auto s = as_scalar(*val, n.location);
            if (!s) return std::nullopt;
            result.push_back(*s);
        }
        bindings_ = saved_bindings;
        return ConstValue{std::move(result)};
    }

    if (func_name == "fold") {
        if (args.size() != 3) {
            error("E152", "fold() requires 3 arguments", n.location);
            return std::nullopt;
        }
        auto arr = as_array(args[0]);
        NodeIndex closure_node = arg_nodes[1];
        auto init = as_scalar(args[2], n.location);
        if (!init) return std::nullopt;

        const Node& closure = ast_->arena[closure_node];
        if (closure.type != NodeType::Closure) {
            error("E202", "fold() second argument must be a closure", n.location);
            return std::nullopt;
        }

        // Extract two parameter names
        std::string param1, param2;
        NodeIndex body = NULL_NODE;
        NodeIndex cchild = closure.first_child;
        while (cchild != NULL_NODE) {
            const Node& cc = ast_->arena[cchild];
            if (cc.type == NodeType::Identifier) {
                std::string pname;
                if (std::holds_alternative<Node::ClosureParamData>(cc.data)) {
                    pname = cc.as_closure_param().name;
                } else if (std::holds_alternative<Node::IdentifierData>(cc.data)) {
                    pname = std::string(interner_->view(cc.as_identifier()));
                } else {
                    body = cchild;
                    break;
                }
                if (param1.empty()) param1 = pname;
                else param2 = pname;
            } else {
                body = cchild;
                break;
            }
            cchild = ast_->arena[cchild].next_sibling;
        }

        if (param1.empty() || param2.empty() || body == NULL_NODE) {
            error("E202", "Invalid closure in fold()", n.location);
            return std::nullopt;
        }

        double acc = *init;
        auto saved_bindings = bindings_;
        for (double elem : arr) {
            bindings_ = saved_bindings;
            bindings_[param1] = ConstValue{acc};
            bindings_[param2] = ConstValue{elem};
            auto val = eval(body);
            if (!val) return std::nullopt;
            auto s = as_scalar(*val, n.location);
            if (!s) return std::nullopt;
            acc = *s;
        }
        bindings_ = saved_bindings;
        return ConstValue{acc};
    }

    if (func_name == "sum") {
        if (args.size() != 1) {
            error("E152", "sum() requires 1 argument", n.location);
            return std::nullopt;
        }
        auto arr = as_array(args[0]);
        double total = 0.0;
        for (double v : arr) total += v;
        return ConstValue{total};
    }

    if (func_name == "len") {
        if (args.size() != 1) {
            error("E152", "len() requires 1 argument", n.location);
            return std::nullopt;
        }
        auto arr = as_array(args[0]);
        return ConstValue{static_cast<double>(arr.size())};
    }

    if (func_name == "mean") {
        if (args.size() != 1) return std::nullopt;
        auto arr = as_array(args[0]);
        if (arr.empty()) return ConstValue{0.0};
        double total = 0.0;
        for (double v : arr) total += v;
        return ConstValue{total / static_cast<double>(arr.size())};
    }

    // Math builtins
    return eval_math_builtin(func_name, args, n.location);
}

std::optional<ConstValue> ConstEvaluator::eval_math_builtin(
    const std::string& name,
    const std::vector<ConstValue>& args,
    SourceLocation loc) {

    // Single-argument math functions
    if (args.size() == 1) {
        auto x = as_scalar(args[0], loc);
        if (!x) return std::nullopt;

        if (name == "sin") return ConstValue{std::sin(*x)};
        if (name == "cos") return ConstValue{std::cos(*x)};
        if (name == "tan") return ConstValue{std::tan(*x)};
        if (name == "asin") return ConstValue{std::asin(*x)};
        if (name == "acos") return ConstValue{std::acos(*x)};
        if (name == "atan") return ConstValue{std::atan(*x)};
        if (name == "sqrt") return ConstValue{std::sqrt(*x)};
        if (name == "abs") return ConstValue{std::abs(*x)};
        if (name == "log") return ConstValue{std::log(*x)};
        if (name == "log2") return ConstValue{std::log2(*x)};
        if (name == "log10") return ConstValue{std::log10(*x)};
        if (name == "exp") return ConstValue{std::exp(*x)};
        if (name == "exp2") return ConstValue{std::exp2(*x)};
        if (name == "floor") return ConstValue{std::floor(*x)};
        if (name == "ceil") return ConstValue{std::ceil(*x)};
        if (name == "round") return ConstValue{std::round(*x)};
        if (name == "trunc") return ConstValue{std::trunc(*x)};
        if (name == "fract") return ConstValue{*x - std::floor(*x)};
        if (name == "sign") return ConstValue{(*x > 0.0) ? 1.0 : ((*x < 0.0) ? -1.0 : 0.0)};
        if (name == "cbrt") return ConstValue{std::cbrt(*x)};
        if (name == "mtof") return ConstValue{440.0 * std::pow(2.0, (*x - 69.0) / 12.0)};
        if (name == "ftom") return ConstValue{69.0 + 12.0 * std::log2(*x / 440.0)};
        // Boolean NOT (desugared from !)
        if (name == "bnot") return ConstValue{*x == 0.0 ? 1.0 : 0.0};
        if (name == "dbtoa") return ConstValue{std::pow(10.0, *x / 20.0)};
        if (name == "atodb") return ConstValue{20.0 * std::log10(*x)};
    }

    // Two-argument math functions
    if (args.size() == 2) {
        auto a = as_scalar(args[0], loc);
        auto b = as_scalar(args[1], loc);
        if (!a || !b) return std::nullopt;

        if (name == "add") return ConstValue{*a + *b};
        if (name == "sub") return ConstValue{*a - *b};
        if (name == "mul") return ConstValue{*a * *b};
        if (name == "div") {
            if (*b == 0.0) {
                error("E200", "Division by zero in const expression", loc);
                return std::nullopt;
            }
            return ConstValue{*a / *b};
        }
        if (name == "pow") return ConstValue{std::pow(*a, *b)};
        if (name == "mod") {
            if (*b == 0.0) {
                error("E200", "Modulo by zero in const expression", loc);
                return std::nullopt;
            }
            return ConstValue{std::fmod(*a, *b)};
        }
        if (name == "atan2") return ConstValue{std::atan2(*a, *b)};
        if (name == "hypot") return ConstValue{std::hypot(*a, *b)};
        if (name == "min") return ConstValue{std::min(*a, *b)};
        if (name == "max") return ConstValue{std::max(*a, *b)};
        // Comparison operators (desugared from ==, !=, <, >, <=, >=)
        if (name == "eq") return ConstValue{*a == *b ? 1.0 : 0.0};
        if (name == "neq") return ConstValue{*a != *b ? 1.0 : 0.0};
        if (name == "lt") return ConstValue{*a < *b ? 1.0 : 0.0};
        if (name == "gt") return ConstValue{*a > *b ? 1.0 : 0.0};
        if (name == "lte") return ConstValue{*a <= *b ? 1.0 : 0.0};
        if (name == "gte") return ConstValue{*a >= *b ? 1.0 : 0.0};
        // Boolean operators (desugared from &&, ||)
        if (name == "band") return ConstValue{(*a != 0.0 && *b != 0.0) ? 1.0 : 0.0};
        if (name == "bor") return ConstValue{(*a != 0.0 || *b != 0.0) ? 1.0 : 0.0};
    }

    // Three-argument functions
    if (args.size() == 3) {
        auto a = as_scalar(args[0], loc);
        auto b = as_scalar(args[1], loc);
        auto c = as_scalar(args[2], loc);
        if (!a || !b || !c) return std::nullopt;

        if (name == "clamp") return ConstValue{std::clamp(*a, *b, *c)};
        if (name == "lerp") return ConstValue{*a + (*b - *a) * *c};
        if (name == "if" || name == "select") {
            return ConstValue{(*a != 0.0) ? *b : *c};
        }
    }

    error("E202", "Cannot evaluate '" + name + "' at compile time with " +
          std::to_string(args.size()) + " argument(s)", loc);
    return std::nullopt;
}

std::optional<ConstValue> ConstEvaluator::eval_const_fn_call(
    const UserFunctionInfo& func,
    const std::vector<ConstValue>& args,
    SourceLocation loc) {

    if (depth_ >= MAX_DEPTH) {
        error("E201", "Recursion depth exceeded in const evaluation (max " +
              std::to_string(MAX_DEPTH) + ")", loc);
        return std::nullopt;
    }

    // Bind parameters
    auto saved_bindings = bindings_;
    int saved_depth = depth_;
    depth_++;

    for (std::size_t i = 0; i < func.params.size(); ++i) {
        if (i < args.size()) {
            bindings_[func.params[i].name] = args[i];
        } else if (func.params[i].default_value.has_value()) {
            bindings_[func.params[i].name] = ConstValue{*func.params[i].default_value};
        } else if (func.params[i].default_node != NULL_NODE) {
            // Expression default: evaluate recursively
            auto expr_result = eval(func.params[i].default_node);
            if (!expr_result) {
                error("E203", "Cannot evaluate default expression for const fn parameter '" +
                      func.params[i].name + "'", loc);
                bindings_ = saved_bindings;
                depth_ = saved_depth;
                return std::nullopt;
            }
            bindings_[func.params[i].name] = *expr_result;
        } else {
            error("E203", "Missing argument for const fn parameter '" +
                  func.params[i].name + "'", loc);
            bindings_ = saved_bindings;
            depth_ = saved_depth;
            return std::nullopt;
        }
    }

    auto result = eval(func.body_node);

    bindings_ = saved_bindings;
    depth_ = saved_depth;

    return result;
}

std::optional<double> ConstEvaluator::as_scalar(const ConstValue& val, SourceLocation loc) {
    if (std::holds_alternative<double>(val)) {
        return std::get<double>(val);
    }
    error("E203", "Expected scalar value, got array", loc);
    return std::nullopt;
}

std::vector<double> ConstEvaluator::as_array(const ConstValue& val) {
    if (std::holds_alternative<std::vector<double>>(val)) {
        return std::get<std::vector<double>>(val);
    }
    // Wrap scalar in single-element array
    return {std::get<double>(val)};
}

void ConstEvaluator::error(const std::string& code, const std::string& message,
                            SourceLocation loc) {
    Diagnostic diag;
    diag.severity = Severity::Error;
    diag.code = code;
    diag.message = message;
    diag.location = loc;
    diagnostics_.push_back(std::move(diag));
}

} // namespace akkado
