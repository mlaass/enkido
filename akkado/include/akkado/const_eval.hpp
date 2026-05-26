#pragma once

#include "ast.hpp"
#include "string_interner.hpp"
#include "symbol_table.hpp"
#include "diagnostics.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace akkado {

/// Compile-time constant evaluator
///
/// Tree-walking interpreter that evaluates const expressions at compile time.
/// Produces ConstValue results (scalar double or vector<double>).
/// Used by codegen to evaluate `const` variable initializers and `const fn` calls.
class ConstEvaluator {
public:
    /// Construct evaluator with AST and symbol table context
    /// @param ast The transformed AST (post-analyzer)
    /// @param symbols Symbol table with const definitions
    /// @param interner Per-compile interner — resolves
    ///        `IdentifierData::name` (SymbolId post-Phase-5) to text.
    ConstEvaluator(const Ast& ast, const SymbolTable& symbols,
                   const StringInterner& interner);

    /// Evaluate an AST node to a compile-time constant
    /// @param node The node to evaluate
    /// @return The constant value, or nullopt on failure
    [[nodiscard]] std::optional<ConstValue> evaluate(NodeIndex node);

    /// Evaluate with parameter bindings (for const fn calls)
    /// @param node The function body node
    /// @param bindings Map from parameter name to value
    /// @return The constant value, or nullopt on failure
    [[nodiscard]] std::optional<ConstValue> evaluate_with_bindings(
        NodeIndex node,
        const std::unordered_map<std::string, ConstValue>& bindings);

    /// Get diagnostics generated during evaluation
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const {
        return diagnostics_;
    }

    /// Check if any errors occurred
    [[nodiscard]] bool has_errors() const {
        return akkado::has_errors(diagnostics_);
    }

    /// Evaluate a const fn call with given arguments
    /// Public so codegen can call it directly for inline const fn dispatch
    std::optional<ConstValue> eval_const_fn_call(const UserFunctionInfo& func,
                                                  const std::vector<ConstValue>& args,
                                                  SourceLocation loc);

private:
    /// Internal eval dispatch
    std::optional<ConstValue> eval(NodeIndex node);

    /// Evaluate a Call node
    std::optional<ConstValue> eval_call(NodeIndex node, const Node& n);

    /// Evaluate a math builtin (sin, cos, pow, etc.)
    std::optional<ConstValue> eval_math_builtin(const std::string& name,
                                                 const std::vector<ConstValue>& args,
                                                 SourceLocation loc);

    /// Extract scalar from ConstValue (error if array)
    std::optional<double> as_scalar(const ConstValue& val, SourceLocation loc);

    /// Extract array from ConstValue (wraps scalar in single-element array)
    std::vector<double> as_array(const ConstValue& val);

    /// Error reporting
    void error(const std::string& code, const std::string& message, SourceLocation loc);

    // Context
    const Ast* ast_;
    const SymbolTable* symbols_;
    const StringInterner* interner_ = nullptr;  // PRD Phase 5 (F12)
    std::vector<Diagnostic> diagnostics_;
    std::string filename_;

    // Parameter bindings for const fn evaluation
    std::unordered_map<std::string, ConstValue> bindings_;

    // Recursion depth tracking
    int depth_ = 0;
    static constexpr int MAX_DEPTH = 16;
};

} // namespace akkado
