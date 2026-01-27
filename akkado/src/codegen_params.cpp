// Parameter exposure codegen implementations
// Handles param(), button(), toggle() for UI auto-generation

#include "akkado/codegen.hpp"
#include "akkado/codegen/codegen.hpp"
#include <cedar/vm/state_pool.hpp>  // For fnv1a_hash_runtime
#include <algorithm>
#include <cmath>

namespace akkado {

using codegen::encode_const_value;
using codegen::unwrap_argument;

// Helper: Extract float constant from argument node
// Returns default_val if argument is not present or not constant

// Helper: Extract float constant from argument node
// Returns default_val if argument is not present or not constant
static float extract_float_arg(const AstArena& arena, NodeIndex arg_node, float default_val) {
    if (arg_node == NULL_NODE) {
        return default_val;
    }

    NodeIndex value_node = unwrap_argument(arena, arg_node);
    const Node& n = arena[value_node];

    if (n.type == NodeType::NumberLit) {
        return static_cast<float>(n.as_number());
    }

    // Try to evaluate if it's a simple expression
    // For MVP, only handle literals
    return default_val;
}

// Helper: Get next sibling argument
static NodeIndex next_arg(const AstArena& arena, NodeIndex arg_node) {
    if (arg_node == NULL_NODE) return NULL_NODE;
    return arena[arg_node].next_sibling;
}

// ============================================================================
// param(name, default, min?, max?) - Continuous parameter (slider)
// ============================================================================

std::uint16_t CodeGenerator::handle_param_call(NodeIndex node, const Node& n) {
    // 1. Extract name argument (must be string literal)
    NodeIndex name_arg = n.first_child;
    if (name_arg == NULL_NODE) {
        error("E150", "param() requires a name argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    NodeIndex name_value = unwrap_argument(ast_->arena, name_arg);
    const Node& name_node = ast_->arena[name_value];

    if (name_node.type != NodeType::StringLit) {
        error("E151", "param() name must be a string literal", name_node.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::string name = std::string(name_node.as_string());
    std::uint32_t name_hash = cedar::fnv1a_hash_runtime(name.data(), name.size());

    // 2. Extract default, min, max values
    float default_val = 0.0f;
    float min_val = 0.0f;
    float max_val = 1.0f;

    NodeIndex arg = next_arg(ast_->arena, name_arg);
    if (arg != NULL_NODE) {
        default_val = extract_float_arg(ast_->arena, arg, 0.0f);
        arg = next_arg(ast_->arena, arg);
    }
    if (arg != NULL_NODE) {
        min_val = extract_float_arg(ast_->arena, arg, 0.0f);
        arg = next_arg(ast_->arena, arg);
    }
    if (arg != NULL_NODE) {
        max_val = extract_float_arg(ast_->arena, arg, 1.0f);
    }

    // 3. Validate range
    if (min_val > max_val) {
        warn("W050", "param() min > max, swapping values", n.location);
        std::swap(min_val, max_val);
    }

    // Clamp default to range
    default_val = std::clamp(default_val, min_val, max_val);

    // 4. Record parameter declaration (deduplicate by name)
    auto existing = std::find_if(param_decls_.begin(), param_decls_.end(),
        [&](const ParamDecl& p) { return p.name == name; });

    if (existing == param_decls_.end()) {
        ParamDecl decl;
        decl.name = name;
        decl.name_hash = name_hash;
        decl.type = ParamType::Continuous;
        decl.default_value = default_val;
        decl.min_value = min_val;
        decl.max_value = max_val;
        decl.source_offset = n.location.offset;
        decl.source_length = n.location.length;
        param_decls_.push_back(std::move(decl));
    } else {
        // Verify consistent declaration
        if (existing->min_value != min_val || existing->max_value != max_val) {
            warn("W051", "param() '" + name + "' redeclared with different range", n.location);
        }
    }

    // 5. Emit fallback value (for when parameter not set in EnvMap)
    std::uint16_t fallback_buf = buffers_.allocate();
    if (fallback_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction push_inst{};
    push_inst.opcode = cedar::Opcode::PUSH_CONST;
    push_inst.out_buffer = fallback_buf;
    push_inst.inputs[0] = 0xFFFF;
    push_inst.inputs[1] = 0xFFFF;
    push_inst.inputs[2] = 0xFFFF;
    push_inst.inputs[3] = 0xFFFF;
    encode_const_value(push_inst, default_val);
    emit(push_inst);

    // 6. Emit ENV_GET instruction
    std::uint16_t out_buf = buffers_.allocate();
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction env_inst{};
    env_inst.opcode = cedar::Opcode::ENV_GET;
    env_inst.out_buffer = out_buf;
    env_inst.inputs[0] = fallback_buf;  // Fallback if param not in EnvMap
    env_inst.inputs[1] = 0xFFFF;
    env_inst.inputs[2] = 0xFFFF;
    env_inst.inputs[3] = 0xFFFF;
    env_inst.state_id = name_hash;       // Parameter lookup key
    emit(env_inst);

    node_buffers_[node] = out_buf;
    return out_buf;
}

// ============================================================================
// button(name) - Momentary button (1 while pressed, 0 otherwise)
// ============================================================================

std::uint16_t CodeGenerator::handle_button_call(NodeIndex node, const Node& n) {
    // 1. Extract name
    NodeIndex name_arg = n.first_child;
    if (name_arg == NULL_NODE) {
        error("E152", "button() requires a name argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    NodeIndex name_value = unwrap_argument(ast_->arena, name_arg);
    const Node& name_node = ast_->arena[name_value];

    if (name_node.type != NodeType::StringLit) {
        error("E153", "button() name must be a string literal", name_node.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::string name = std::string(name_node.as_string());
    std::uint32_t name_hash = cedar::fnv1a_hash_runtime(name.data(), name.size());

    // 2. Record declaration (deduplicate)
    auto existing = std::find_if(param_decls_.begin(), param_decls_.end(),
        [&](const ParamDecl& p) { return p.name == name; });

    if (existing == param_decls_.end()) {
        ParamDecl decl;
        decl.name = name;
        decl.name_hash = name_hash;
        decl.type = ParamType::Button;
        decl.default_value = 0.0f;  // Buttons default to not pressed
        decl.min_value = 0.0f;
        decl.max_value = 1.0f;
        decl.source_offset = n.location.offset;
        decl.source_length = n.location.length;
        param_decls_.push_back(std::move(decl));
    }

    // 3. Emit fallback (0.0 = not pressed)
    std::uint16_t fallback_buf = buffers_.allocate();
    if (fallback_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction push_inst{};
    push_inst.opcode = cedar::Opcode::PUSH_CONST;
    push_inst.out_buffer = fallback_buf;
    push_inst.inputs[0] = 0xFFFF;
    push_inst.inputs[1] = 0xFFFF;
    push_inst.inputs[2] = 0xFFFF;
    push_inst.inputs[3] = 0xFFFF;
    encode_const_value(push_inst, 0.0f);
    emit(push_inst);

    // 4. Emit ENV_GET
    std::uint16_t out_buf = buffers_.allocate();
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction env_inst{};
    env_inst.opcode = cedar::Opcode::ENV_GET;
    env_inst.out_buffer = out_buf;
    env_inst.inputs[0] = fallback_buf;
    env_inst.inputs[1] = 0xFFFF;
    env_inst.inputs[2] = 0xFFFF;
    env_inst.inputs[3] = 0xFFFF;
    env_inst.state_id = name_hash;
    emit(env_inst);

    node_buffers_[node] = out_buf;
    return out_buf;
}

// ============================================================================
// toggle(name, default?) - Boolean toggle (click to flip)
// ============================================================================

std::uint16_t CodeGenerator::handle_toggle_call(NodeIndex node, const Node& n) {
    // 1. Extract name
    NodeIndex name_arg = n.first_child;
    if (name_arg == NULL_NODE) {
        error("E154", "toggle() requires a name argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    NodeIndex name_value = unwrap_argument(ast_->arena, name_arg);
    const Node& name_node = ast_->arena[name_value];

    if (name_node.type != NodeType::StringLit) {
        error("E155", "toggle() name must be a string literal", name_node.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::string name = std::string(name_node.as_string());
    std::uint32_t name_hash = cedar::fnv1a_hash_runtime(name.data(), name.size());

    // 2. Extract default (optional, defaults to 0)
    float default_val = 0.0f;
    NodeIndex arg = next_arg(ast_->arena, name_arg);
    if (arg != NULL_NODE) {
        float raw_default = extract_float_arg(ast_->arena, arg, 0.0f);
        default_val = (raw_default > 0.5f) ? 1.0f : 0.0f;  // Normalize to boolean
    }

    // 3. Record declaration (deduplicate)
    auto existing = std::find_if(param_decls_.begin(), param_decls_.end(),
        [&](const ParamDecl& p) { return p.name == name; });

    if (existing == param_decls_.end()) {
        ParamDecl decl;
        decl.name = name;
        decl.name_hash = name_hash;
        decl.type = ParamType::Toggle;
        decl.default_value = default_val;
        decl.min_value = 0.0f;
        decl.max_value = 1.0f;
        decl.source_offset = n.location.offset;
        decl.source_length = n.location.length;
        param_decls_.push_back(std::move(decl));
    }

    // 4. Emit fallback
    std::uint16_t fallback_buf = buffers_.allocate();
    if (fallback_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction push_inst{};
    push_inst.opcode = cedar::Opcode::PUSH_CONST;
    push_inst.out_buffer = fallback_buf;
    push_inst.inputs[0] = 0xFFFF;
    push_inst.inputs[1] = 0xFFFF;
    push_inst.inputs[2] = 0xFFFF;
    push_inst.inputs[3] = 0xFFFF;
    encode_const_value(push_inst, default_val);
    emit(push_inst);

    // 5. Emit ENV_GET
    std::uint16_t out_buf = buffers_.allocate();
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction env_inst{};
    env_inst.opcode = cedar::Opcode::ENV_GET;
    env_inst.out_buffer = out_buf;
    env_inst.inputs[0] = fallback_buf;
    env_inst.inputs[1] = 0xFFFF;
    env_inst.inputs[2] = 0xFFFF;
    env_inst.inputs[3] = 0xFFFF;
    env_inst.state_id = name_hash;
    emit(env_inst);

    node_buffers_[node] = out_buf;
    return out_buf;
}

// ============================================================================
// dropdown(name, opt1, opt2, ...) - Selection dropdown parameter
// Returns integer index (0, 1, 2, ...) of selected option
// ============================================================================

std::uint16_t CodeGenerator::handle_select_call(NodeIndex node, const Node& n) {
    // 1. Extract name
    NodeIndex name_arg = n.first_child;
    if (name_arg == NULL_NODE) {
        error("E156", "dropdown() requires a name argument", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    NodeIndex name_value = unwrap_argument(ast_->arena, name_arg);
    const Node& name_node = ast_->arena[name_value];

    if (name_node.type != NodeType::StringLit) {
        error("E157", "dropdown() name must be a string literal", name_node.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    std::string name = std::string(name_node.as_string());
    std::uint32_t name_hash = cedar::fnv1a_hash_runtime(name.data(), name.size());

    // 2. Extract option strings
    std::vector<std::string> options;
    NodeIndex arg = next_arg(ast_->arena, name_arg);

    while (arg != NULL_NODE) {
        NodeIndex opt_value = unwrap_argument(ast_->arena, arg);
        const Node& opt_node = ast_->arena[opt_value];

        if (opt_node.type != NodeType::StringLit) {
            error("E158", "dropdown() options must be string literals", opt_node.location);
            return BufferAllocator::BUFFER_UNUSED;
        }

        options.push_back(std::string(opt_node.as_string()));
        arg = next_arg(ast_->arena, arg);
    }

    // 3. Validate at least one option
    if (options.empty()) {
        error("E159", "dropdown() requires at least one option", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    // 4. Record declaration (deduplicate)
    auto existing = std::find_if(param_decls_.begin(), param_decls_.end(),
        [&](const ParamDecl& p) { return p.name == name; });

    if (existing == param_decls_.end()) {
        ParamDecl decl;
        decl.name = name;
        decl.name_hash = name_hash;
        decl.type = ParamType::Select;
        decl.default_value = 0.0f;  // First option is default
        decl.min_value = 0.0f;
        decl.max_value = static_cast<float>(options.size() - 1);
        decl.options = std::move(options);
        decl.source_offset = n.location.offset;
        decl.source_length = n.location.length;
        param_decls_.push_back(std::move(decl));
    }

    // 5. Emit fallback (0.0 = first option)
    std::uint16_t fallback_buf = buffers_.allocate();
    if (fallback_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction push_inst{};
    push_inst.opcode = cedar::Opcode::PUSH_CONST;
    push_inst.out_buffer = fallback_buf;
    push_inst.inputs[0] = 0xFFFF;
    push_inst.inputs[1] = 0xFFFF;
    push_inst.inputs[2] = 0xFFFF;
    push_inst.inputs[3] = 0xFFFF;
    encode_const_value(push_inst, 0.0f);
    emit(push_inst);

    // 6. Emit ENV_GET
    std::uint16_t out_buf = buffers_.allocate();
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return BufferAllocator::BUFFER_UNUSED;
    }

    cedar::Instruction env_inst{};
    env_inst.opcode = cedar::Opcode::ENV_GET;
    env_inst.out_buffer = out_buf;
    env_inst.inputs[0] = fallback_buf;
    env_inst.inputs[1] = 0xFFFF;
    env_inst.inputs[2] = 0xFFFF;
    env_inst.inputs[3] = 0xFFFF;
    env_inst.state_id = name_hash;
    emit(env_inst);

    node_buffers_[node] = out_buf;
    return out_buf;
}

}  // namespace akkado
