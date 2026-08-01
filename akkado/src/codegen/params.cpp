// Parameter exposure codegen — param(), button(), toggle(), dropdown() for
// UI auto-generation. One data-driven emitter (PRD prd-codegen-sprawl-cleanup
// Phase 6): each builtin's BUILTIN_FUNCTIONS entry carries a ParamMeta with
// its control kind and diagnostic codes; the per-kind differences (range
// handling, boolean normalization, option collection) are one switch in the
// middle of the shared skeleton.

#include "akkado/codegen.hpp"
#include "akkado/compile_context.hpp"
#include "akkado/string_interner.hpp"
#include "akkado/codegen/codegen.hpp"
#include "akkado/codegen/instruction_builder.hpp"
#include "akkado/builtins.hpp"
#include <cedar/vm/state_pool.hpp>  // For fnv1a_hash_runtime
#include <algorithm>
#include <cmath>

namespace akkado {

using codegen::unwrap_argument;

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

// The canonical param-control shape:
//   name extraction → per-kind value/option extraction (switch) →
//   ParamDecl record (deduplicated by name) → PUSH_CONST fallback →
//   ENV_GET keyed by fnv1a(name)
TypedValue CodeGenerator::emit_param(NodeIndex node, const Node& n) {
    const std::string func_name{ctx_->interner->view(n.as_identifier())};
    const BuiltinInfo* info = lookup_builtin(func_name);
    if (info == nullptr || !info->param_meta.has_value()) {
        error("E107", "Unknown param builtin: " + func_name, n.location);
        return TypedValue::error_val();
    }
    const ParamMeta& meta = *info->param_meta;

    // 1. Extract name argument (must be string literal)
    NodeIndex name_arg = n.first_child;
    if (name_arg == NULL_NODE) {
        error(meta.err_missing,
              std::string(meta.name) + "() requires a name argument",
              n.location);
        return TypedValue::void_val();
    }

    NodeIndex name_value = unwrap_argument(ast_->arena, name_arg);
    const Node& name_node = ast_->arena[name_value];

    if (name_node.type != NodeType::StringLit) {
        error(meta.err_not_string,
              std::string(meta.name) + "() name must be a string literal",
              name_node.location);
        return TypedValue::void_val();
    }

    std::string name = std::string(name_node.as_string());
    std::uint32_t name_hash = cedar::fnv1a_hash_runtime(name.data(), name.size());

    // 2. Per-kind value / option extraction.
    float default_val = 0.0f;
    float min_val = 0.0f;
    float max_val = 1.0f;
    std::vector<std::string> options;

    switch (meta.decl_kind) {
        case ParamType::Continuous: {
            // param(name, default?, min?, max?)
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
            if (min_val > max_val) {
                warn("W050", "param() min > max, swapping values", n.location);
                std::swap(min_val, max_val);
            }
            default_val = std::clamp(default_val, min_val, max_val);
            break;
        }
        case ParamType::Button:
            // button(name) — defaults to not pressed.
            break;
        case ParamType::Toggle: {
            // toggle(name, default?) — normalize to boolean.
            NodeIndex arg = next_arg(ast_->arena, name_arg);
            if (arg != NULL_NODE) {
                float raw_default = extract_float_arg(ast_->arena, arg, 0.0f);
                default_val = (raw_default > 0.5f) ? 1.0f : 0.0f;
            }
            break;
        }
        case ParamType::Select: {
            // dropdown(name, opt1, opt2, ...) — returns the selected index.
            NodeIndex arg = next_arg(ast_->arena, name_arg);
            while (arg != NULL_NODE) {
                NodeIndex opt_value = unwrap_argument(ast_->arena, arg);
                const Node& opt_node = ast_->arena[opt_value];
                if (opt_node.type != NodeType::StringLit) {
                    error("E158", "dropdown() options must be string literals",
                          opt_node.location);
                    return TypedValue::void_val();
                }
                options.push_back(std::string(opt_node.as_string()));
                arg = next_arg(ast_->arena, arg);
            }
            if (options.empty()) {
                error("E159", "dropdown() requires at least one option",
                      n.location);
                return TypedValue::void_val();
            }
            max_val = static_cast<float>(options.size() - 1);
            break;
        }
    }

    // 3. Record parameter declaration (deduplicate by name)
    auto existing = std::find_if(param_decls_.begin(), param_decls_.end(),
        [&](const ParamDecl& p) { return p.name == name; });

    if (existing == param_decls_.end()) {
        ParamDecl decl;
        decl.name = name;
        decl.name_hash = name_hash;
        decl.type = meta.decl_kind;
        decl.default_value = default_val;
        decl.min_value = min_val;
        decl.max_value = max_val;
        decl.options = std::move(options);
        decl.source_offset = n.location.offset;
        decl.source_length = n.location.length;
        param_decls_.push_back(std::move(decl));
    } else if (meta.decl_kind == ParamType::Continuous) {
        // Verify consistent declaration (sliders only, historically).
        if (existing->min_value != min_val || existing->max_value != max_val) {
            warn("W051", "param() '" + name + "' redeclared with different range",
                 n.location);
        }
    }

    // 4. Emit fallback value (for when parameter not set in EnvMap)
    const std::uint16_t fallback_buf =
        codegen::InstructionBuilder(cedar::Opcode::PUSH_CONST)
            .const_value(default_val)
            .emit(*this, n.location);
    if (fallback_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    // 5. Emit ENV_GET instruction
    const std::uint16_t out_buf =
        codegen::InstructionBuilder(cedar::Opcode::ENV_GET)
            .input(0, fallback_buf)  // Fallback if param not in EnvMap
            .input(4, 0)             // preserve historical zero-init slot
            .state_id(name_hash)     // Parameter lookup key
            .emit(*this, n.location);
    if (out_buf == BufferAllocator::BUFFER_UNUSED) {
        return TypedValue::void_val();
    }

    return cache_and_return(node, TypedValue::signal(out_buf));
}

}  // namespace akkado
