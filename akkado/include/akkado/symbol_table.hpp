#pragma once

#include "builtins.hpp"
#include "ast.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace akkado {

/// Information about a user-defined function parameter
struct FunctionParamInfo {
    std::string name;
    std::optional<double> default_value;
};

/// Information about a user-defined function
struct UserFunctionInfo {
    std::string name;
    std::vector<FunctionParamInfo> params;
    NodeIndex body_node;  // Index of function body in AST
    NodeIndex def_node;   // Index of FunctionDef node (for inlining)
};

// Forward-declare hash function (same as Cedar's FNV-1a)
inline std::uint32_t fnv1a_hash(std::string_view str) noexcept {
    std::uint32_t hash = 2166136261u;  // FNV-1a 32-bit offset basis
    for (char c : str) {
        hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        hash *= 16777619u;  // FNV-1a 32-bit prime
    }
    return hash;
}

/// Symbol kinds
enum class SymbolKind : std::uint8_t {
    Variable,       // User-defined variable (scalar)
    Builtin,        // Built-in function
    Parameter,      // Closure parameter
    UserFunction,   // User-defined function (fn)
    Pattern,        // Pattern variable (pat(), seq(), etc.)
    Array,          // Array value
    FunctionValue,  // Function as value (lambda or fn reference)
    Record,         // Record value (structured data with named fields)
};

/// Information about a pattern variable
struct PatternInfo {
    NodeIndex pattern_node;        // Index of MiniLiteral node in transformed AST
    bool is_sample_pattern;        // true if pattern contains samples (not pitches)
};

/// Information about an array variable
struct ArrayInfo {
    std::vector<std::uint16_t> buffer_indices;  // Populated during codegen
    NodeIndex source_node;                       // Original ArrayLit node
    std::size_t element_count;                   // Cached length
};

/// Information about a captured variable (read-only closure capture)
struct CaptureInfo {
    std::string name;
    std::uint16_t buffer_index;
};

/// Information about a function value (lambda or fn reference)
struct FunctionRef {
    NodeIndex closure_node;                      // Points to Closure or FunctionDef body
    std::vector<FunctionParamInfo> params;       // Parameter info
    std::vector<CaptureInfo> captures;           // Captured variables (read-only)
    bool is_user_function;                       // true if from `fn`
    std::string user_function_name;              // For user functions
};

/// Information about a record field
struct RecordFieldInfo {
    std::string name;                            // Field name
    std::uint16_t buffer_index;                  // Buffer index for this field's value
    SymbolKind field_kind;                       // Kind of value (Variable, Record, etc.)
    // For nested records, we store the nested type info
    std::shared_ptr<struct RecordTypeInfo> nested_record_type;
};

/// Information about a record type
struct RecordTypeInfo {
    std::vector<RecordFieldInfo> fields;         // Field definitions in declaration order
    NodeIndex source_node;                       // Original RecordLit node

    /// Find a field by name, returns nullptr if not found
    const RecordFieldInfo* find_field(std::string_view name) const {
        for (const auto& field : fields) {
            if (field.name == name) {
                return &field;
            }
        }
        return nullptr;
    }

    /// Get list of all field names (for error messages)
    std::vector<std::string> field_names() const {
        std::vector<std::string> names;
        names.reserve(fields.size());
        for (const auto& field : fields) {
            names.push_back(field.name);
        }
        return names;
    }
};

/// Symbol entry in the symbol table
struct Symbol {
    SymbolKind kind;
    std::uint32_t name_hash;       // FNV-1a hash of name
    std::string name;              // Original name (for error messages)
    std::uint16_t buffer_index;    // Allocated buffer for variables/params

    // Only valid if kind == Builtin
    BuiltinInfo builtin;

    // Only valid if kind == UserFunction
    UserFunctionInfo user_function;

    // Only valid if kind == Pattern
    PatternInfo pattern;

    // Only valid if kind == Array
    ArrayInfo array;

    // Only valid if kind == FunctionValue
    FunctionRef function_ref;

    // Only valid if kind == Record
    std::shared_ptr<RecordTypeInfo> record_type;
};

/// Scoped symbol table with lexical scoping
class SymbolTable {
public:
    SymbolTable();

    /// Push a new scope (entering block/closure)
    void push_scope();

    /// Pop the current scope (leaving block/closure)
    void pop_scope();

    /// Get current scope depth (0 = global)
    [[nodiscard]] std::size_t scope_depth() const { return scopes_.size(); }

    /// Define a symbol in the current scope
    /// Returns false if symbol already defined in current scope
    bool define(const Symbol& symbol);

    /// Define a variable and allocate a buffer for it
    bool define_variable(std::string_view name, std::uint16_t buffer_index);

    /// Define a closure parameter
    bool define_parameter(std::string_view name, std::uint16_t buffer_index);

    /// Define a user function
    bool define_function(const UserFunctionInfo& func_info);

    /// Define a pattern variable
    bool define_pattern(std::string_view name, const PatternInfo& pattern_info);

    /// Define an array variable
    bool define_array(std::string_view name, const ArrayInfo& array_info);

    /// Define a function value (lambda or fn reference)
    bool define_function_value(std::string_view name, const FunctionRef& func_ref);

    /// Define a record variable
    bool define_record(std::string_view name, std::shared_ptr<RecordTypeInfo> record_type);

    /// Lookup a symbol by name (searches all scopes, innermost first)
    [[nodiscard]] std::optional<Symbol> lookup(std::string_view name) const;

    /// Lookup by hash (faster for repeated lookups)
    [[nodiscard]] std::optional<Symbol> lookup(std::uint32_t name_hash) const;

    /// Check if a name is defined in the current scope only
    [[nodiscard]] bool is_defined_in_current_scope(std::string_view name) const;

    /// Update function body/def node indices after AST transformation
    void update_function_nodes(const std::unordered_map<NodeIndex, NodeIndex>& node_map);

private:
    /// Each scope is a hash map from name_hash to Symbol
    std::vector<std::unordered_map<std::uint32_t, Symbol>> scopes_;

    /// Pre-populate with builtins
    void register_builtins();
};

} // namespace akkado
