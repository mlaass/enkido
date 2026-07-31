#pragma once

#include "builtins.hpp"
#include "ast.hpp"
#include "string_interner.hpp"
#include "typed_value.hpp"
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
    std::optional<std::string> default_string;  // String default for match dispatch
    NodeIndex default_node = NULL_NODE;          // AST node for default literal (for param_literals_)
    bool is_rest = false;                        // true for ...param (variadic rest)
    // Phase 3b: function-parameter destructure `fn f({x, y [= default]})`.
    // When set, `name` is a synthetic placeholder (`__destr_param_<N>`) and
    // the per-field bindings live in `destructure_fields`. The corresponding
    // AST node is `DestructureParam`, not `Identifier`.
    bool is_destructure = false;
    std::vector<DestructureField> destructure_fields;
    // PRD prd-parameter-type-annotations §4.4: resolved type annotation on
    // user-fn parameters. Defaults to Any (un-annotated). Read by
    // handle_user_function_call to branch on `: stream` / `: signal`.
    ParamValueType annotated_type = ParamValueType::Any;
};

/// Information about a user-defined function
struct UserFunctionInfo {
    std::string name;
    std::vector<FunctionParamInfo> params;
    NodeIndex body_node;  // Index of function body in AST
    NodeIndex def_node;   // Index of FunctionDef node (for inlining)
    bool has_rest_param = false;  // true if last param is ...rest
    bool returns_closure = false; // true if body is explicitly a Closure in source
    bool is_const = false;  // true for const fn (compile-time evaluable)
    bool is_inline = false; // true for #inline fn (forces per-site inlining)
    // Phase 4: true when this definition comes from the embedded stdlib/prelude
    // (`<stdlib>` or `<stdlib/*.ak>`). A user-source definition shadows the
    // whole stdlib overload set for that name (the documented "user code can
    // shadow these" idiom); only same-origin definitions accumulate.
    bool is_stdlib = false;
};

/// Compile-time constant value (scalar or array)
using ConstValue = std::variant<double, std::vector<double>>;

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
    Module,         // Namespace import binding (import "x" as m)
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
    std::vector<FunctionRef> compose_chain;      // For compose(): chain of functions to apply in sequence
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
    SymbolId name_id = NULL_SYMBOL; // PRD Phase 5 (F12): interner id (was name_hash)
    std::string name;              // Original name (for error messages)
    std::uint16_t buffer_index;    // Allocated buffer for variables/params

    // Const variable support
    bool is_const = false;
    std::optional<ConstValue> const_value;

    // Multi-buffer support: all buffers if value is multi-buffer (empty = single)
    std::vector<std::uint16_t> multi_buffers;

    // Only valid if kind == Builtin
    BuiltinInfo builtin;

    // Only valid if kind == UserFunction. Phase 4 (overload resolution): a
    // name owns an ordered list of overloads. Distinct param signatures
    // accumulate; a same-signature redefinition replaces in place (last-wins
    // within one compilation). This is the single source of truth — there is
    // no separate "primary" copy to keep in sync (the analyzer's AST-clone
    // remap mutates body_node/def_node in place, so a duplicate would rot).
    std::vector<UserFunctionInfo> overloads;

    // The first-declared overload — the warn+fallback target for call sites
    // that cannot select an overload by argument type (named args, `_`
    // partial application, spread, or the bare name used as a value) and for
    // single-body legacy readers. Only call when kind == UserFunction (then
    // `overloads` is guaranteed non-empty).
    const UserFunctionInfo& primary_overload() const { return overloads.front(); }

    // Only valid if kind == Pattern
    PatternInfo pattern;

    // Only valid if kind == Array
    ArrayInfo array;

    // Only valid if kind == FunctionValue
    FunctionRef function_ref;

    // Only valid if kind == Record
    std::shared_ptr<RecordTypeInfo> record_type;

    // Full typed value from codegen (for pipe bindings, patterns, records)
    std::optional<TypedValue> typed_value;

    // Phase 4b: set when this Variable is bound to a record-valued state cell
    // (`v = state({...})` directly, or `t = v` aliasing such a cell). Lets the
    // analyzer's FieldAccess validation skip the E061 ("non-record value") check
    // for state-cell receivers and let codegen route the read sugar instead.
    bool is_state_cell = false;

    // Canonical path of originating module (empty if local)
    std::string origin_module;

    // Only valid if kind == Module: canonical path of the module
    std::string module_path;
};

/// Process-shared builtin scope (hardening PRD Phase 3). Immutable map from
/// builtin/alias name to a prototype Symbol (`name_id == NULL_SYMBOL` — ids
/// are per-compile; `SymbolTable::lookup` patches the caller's id onto the
/// returned copy). Built once on first use; lives for the process lifetime.
/// Host-extension builtins are not included — they are consulted live so
/// registrations between compiles are honored.
const std::unordered_map<std::string_view, Symbol>& builtin_scope();

/// Outcome of accumulating a user-function definition (Phase 4 overloading).
enum class DefineFunctionResult {
    Added,                  // first definition of this name in the current scope
    Accumulated,            // distinct signature appended to an existing set
    ReplacedSameSignature,  // same-signature redefinition replaced in place
};

/// Scoped symbol table with lexical scoping
///
/// PRD prd-parser-codegen-correctness.md Phase 5 (F12): the internal
/// scopes_ map is now keyed on `SymbolId` (interner-assigned u32) so
/// lookups skip the per-call FNV-1a recomputation that the old
/// `Symbol.name_hash` path required. Callers that already hold a
/// `SymbolId` (e.g. from `Node::as_identifier()`) hit the fast path.
/// Callers that hand a `std::string_view` route through a convenience
/// overload that interns first.
///
/// The default ctor leaves the table empty (one initial scope, no
/// builtins). Production code passes a `StringInterner&` so
/// `register_builtins()` can intern each builtin name. Unit tests
/// that exercise scope mechanics keep using the default ctor.
class SymbolTable {
public:
    SymbolTable();
    /// Construct with an interner and pre-register all builtin
    /// functions + aliases.
    explicit SymbolTable(StringInterner& interner);

    /// Attach an interner post-construction. Required before any
    /// `string_view`-based define/lookup overload is called.
    void set_interner(StringInterner& interner) { interner_ = &interner; }

    /// Returns the attached interner, or nullptr if none. Tooling that
    /// renders diagnostics uses this to resolve `Symbol.name_id` ->
    /// view (Symbol.name remains the authoritative diagnostic text).
    [[nodiscard]] const StringInterner* interner() const { return interner_; }

    /// Arm builtin resolution (hardening PRD Phase 3): attaches the interner
    /// and enables the lookup fallback to the process-shared builtin_scope().
    /// No per-table inserts happen anymore. Idempotent.
    void register_builtins(StringInterner& interner);

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

    /// Define a user function. Phase 4: a same-name definition with a *distinct*
    /// signature (arity + ordered param annotated-type list + per-param
    /// required-ness) accumulates as a new overload; a *same*-signature
    /// definition replaces the existing one in place (last-wins). The return
    /// value lets the analyzer warn only on same-signature replacement.
    DefineFunctionResult define_function(const UserFunctionInfo& func_info);

    /// Define a pattern variable
    bool define_pattern(std::string_view name, const PatternInfo& pattern_info);

    /// Define an array variable
    bool define_array(std::string_view name, const ArrayInfo& array_info);

    /// Define a function value (lambda or fn reference)
    bool define_function_value(std::string_view name, const FunctionRef& func_ref);

    /// Define a record variable
    bool define_record(std::string_view name, std::shared_ptr<RecordTypeInfo> record_type);

    /// Define a const variable with a compile-time value
    bool define_const_variable(std::string_view name, const ConstValue& value);

    /// Define a const variable placeholder (value not yet known)
    bool define_const_placeholder(std::string_view name);

    /// Lookup a symbol by name (searches all scopes, innermost first).
    /// Requires an attached interner; routes through `find()` so an
    /// absent name does not bloat the interner.
    [[nodiscard]] std::optional<Symbol> lookup(std::string_view name) const;

    /// Lookup by SymbolId (PRD Phase 5 — no FNV recomputation).
    [[nodiscard]] std::optional<Symbol> lookup(SymbolId id) const;

    /// Check if a name is defined in the current scope only
    [[nodiscard]] bool is_defined_in_current_scope(std::string_view name) const;

    /// Update function body/def node indices after AST transformation
    void update_function_nodes(const std::unordered_map<NodeIndex, NodeIndex>& node_map);

    /// Define a module namespace binding (import "x" as alias)
    bool define_module(std::string_view alias, std::string_view canonical_path);

    /// Look up a symbol in a specific module's hidden symbols
    [[nodiscard]] std::optional<Symbol> lookup_in_module(
        std::string_view module_path, std::string_view name) const;

    /// Move a symbol from scopes_ to hidden_symbols_ for the given module
    void hide_symbol(std::string_view name, std::string_view module_path);

    /// Read-only access to the global (depth=0) scope. Used by tooling that
    /// needs to enumerate top-level bindings (e.g. shape-index serializer
    /// for editor autocomplete). Returns an empty reference if the table is
    /// in an unexpected state — callers should treat empty as "no globals".
    [[nodiscard]] const std::unordered_map<SymbolId, Symbol>& globals() const {
        static const std::unordered_map<SymbolId, Symbol> empty;
        return scopes_.empty() ? empty : scopes_.front();
    }

private:
    /// Each scope is a hash map from SymbolId to Symbol (PRD Phase 5).
    std::vector<std::unordered_map<SymbolId, Symbol>> scopes_;

    /// Hidden symbols per module (for namespace imports).
    /// Key: module canonical path → inner map: SymbolId → Symbol
    std::unordered_map<std::string, std::unordered_map<SymbolId, Symbol>> hidden_symbols_;

    /// Per-compile string interner (non-owning). Required for
    /// string_view-based define/lookup overloads.
    StringInterner* interner_ = nullptr;

    /// Phase 3: when true (interner ctor / register_builtins), lookups that
    /// miss every scope fall back to the shared builtin_scope() + host
    /// extensions. The default ctor leaves it false so scope-mechanics tests
    /// see a genuinely empty table.
    bool use_builtins_ = false;

    /// Resolve `name` against builtin_scope() + host extensions, patching
    /// `id` (the per-compile SymbolId, or NULL_SYMBOL if not interned) onto
    /// the returned copy.
    std::optional<Symbol> lookup_builtin(std::string_view name, SymbolId id) const;
};

} // namespace akkado
