#pragma once

#include "ast.hpp"
#include "diagnostics.hpp"
#include "symbol_table.hpp"
#include <cedar/vm/instruction.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace akkado {

/// Result of code generation
struct CodeGenResult {
    std::vector<cedar::Instruction> instructions;
    std::vector<Diagnostic> diagnostics;
    bool success = false;
};

/// Buffer allocator for code generation
/// Simple linear allocation with no reuse (MVP)
class BufferAllocator {
public:
    static constexpr std::uint16_t MAX_BUFFERS = 256;
    static constexpr std::uint16_t BUFFER_UNUSED = 0xFFFF;

    BufferAllocator() = default;

    /// Allocate a new buffer
    /// Returns BUFFER_UNUSED if pool exhausted
    [[nodiscard]] std::uint16_t allocate();

    /// Get current allocation count
    [[nodiscard]] std::uint16_t count() const { return next_; }

    /// Check if any buffers available
    [[nodiscard]] bool has_available() const { return next_ < MAX_BUFFERS; }

private:
    std::uint16_t next_ = 0;
};

/// Code generator: converts analyzed AST to Cedar bytecode
class CodeGenerator {
public:
    /// Generate bytecode from analyzed AST
    /// @param ast The transformed AST (after pipe rewriting)
    /// @param symbols Symbol table from semantic analysis
    /// @param filename Filename for error reporting
    CodeGenResult generate(const Ast& ast, SymbolTable& symbols,
                          std::string_view filename = "<input>");

private:
    /// Visit AST node and emit instructions
    /// Returns the buffer index containing this node's result
    std::uint16_t visit(NodeIndex node);

    /// Emit a single instruction
    void emit(const cedar::Instruction& inst);

    /// Generate semantic ID from path
    [[nodiscard]] std::uint32_t compute_state_id() const;

    /// Push/pop semantic path for nested structures
    void push_path(std::string_view segment);
    void pop_path();

    /// Error helpers
    void error(const std::string& code, const std::string& message, SourceLocation loc);

    // Context
    const Ast* ast_ = nullptr;
    SymbolTable* symbols_ = nullptr;
    BufferAllocator buffers_;
    std::vector<cedar::Instruction> instructions_;
    std::vector<Diagnostic> diagnostics_;
    std::string filename_;

    // Semantic path tracking for state_id generation
    std::vector<std::string> path_stack_;
    std::uint32_t anonymous_counter_ = 0;

    // Track call counts per stateful function for unique state_ids
    std::unordered_map<std::string, std::uint32_t> call_counters_;

    // Map from AST node index to output buffer index
    std::unordered_map<NodeIndex, std::uint16_t> node_buffers_;
};

} // namespace akkado
