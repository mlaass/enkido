// User state cells (state/get/set) — Phase 3 of
// prd-userspace-state-and-edge-primitives.md, extended in Phase 4a of
// prd-records-system-unification.md to support record-valued cells.
//
// state(init) lowers to STATE_OP rate=0 and returns a StateCell TypedValue
// that carries the slot's state_id (FNV-1a path-hash, identical scheme to
// every other stateful builtin). .get() and .set() consume that handle and
// emit STATE_OP rate=1 (load) and rate=2 (store) respectively.
//
// state/get/set are name-reserved at the parser level — users cannot define
// closures with these names. See parser.cpp.
//
// Record-valued cells (Phase 4a): records have no runtime representation,
// so a record-valued cell is fanned out into one scalar STATE_OP slot per
// field, keyed by per-field path hashes (parent_path/field_name). The
// returned StateCell TypedValue carries a RecordPayload of per-field
// sub-cells; get() and set() iterate over those sub-cells in sorted-name
// order to keep codegen deterministic across compiles (and hence keep
// hot-swap diffs stable).

#include "akkado/codegen.hpp"
#include "akkado/builtins.hpp"
#include <cedar/vm/instruction.hpp>
#include <cedar/dsp/constants.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace akkado {

namespace {

// Walk past an optional Argument wrapper to the underlying expression node.
NodeIndex unwrap_argument(const AstArena& arena, NodeIndex idx) {
    if (idx == NULL_NODE) return NULL_NODE;
    const Node& n = arena[idx];
    if (n.type == NodeType::Argument) return n.first_child;
    return idx;
}

// Set-equality on field names (used for E189 shape mismatch detection).
bool record_shapes_match(const RecordPayload& a, const RecordPayload& b) {
    if (a.fields.size() != b.fields.size()) return false;
    for (const auto& [name, _] : a.fields) {
        if (b.fields.find(name) == b.fields.end()) return false;
    }
    return true;
}

// Render a record's field-name set as "{x, y, z}" for E189 messages.
// Sorted alphabetically so the rendering is stable.
std::string shape_csv(const RecordPayload& r) {
    std::vector<std::string> names;
    names.reserve(r.fields.size());
    for (const auto& [name, _] : r.fields) names.push_back(name);
    std::sort(names.begin(), names.end());
    std::string out = "{";
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i) out += ", ";
        out += names[i];
    }
    out += "}";
    return out;
}

// Sorted field-name list for deterministic fan-out.
std::vector<std::string> sorted_field_names(const RecordPayload& r) {
    std::vector<std::string> names;
    names.reserve(r.fields.size());
    for (const auto& [name, _] : r.fields) names.push_back(name);
    std::sort(names.begin(), names.end());
    return names;
}

// StateCell TypedValue's `cell_state_id` is meaningless for record cells —
// callers identify record cells by `tv.record != nullptr`. Use a sentinel
// so any stray code that reads `cell_state_id` on a record cell hits a
// deterministic value (and likely a missing StatePool slot) rather than
// silently colliding with state_id 0.
constexpr std::uint32_t RECORD_CELL_SENTINEL_ID = 0xFFFFFFFFu;

}  // namespace

TypedValue CodeGenerator::handle_state_call(NodeIndex node, const Node& n) {
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) {
        error("E120", "state() requires exactly 1 argument (initial value)", n.location);
        return TypedValue::error_val();
    }
    NodeIndex init_node = unwrap_argument(ast_->arena, arg);
    if (init_node == NULL_NODE) {
        error("E120", "state() requires an initial value", n.location);
        return TypedValue::error_val();
    }

    // Allocate this call site's path segment via the same path-hash mechanism
    // used by every other stateful builtin. Each `state(...)` source position
    // gets a distinct parent path; inside a closure called from multiple
    // sites, the surrounding push_path("closure_name#N") gives each invocation
    // its own slot — which is exactly the semantic users expect for
    // `make_counter()`. For scalar cells the parent path *is* the state_id;
    // for record cells the parent path becomes the prefix of each per-field
    // state_id (parent/field_name).
    std::uint32_t count = call_counters_["state"]++;
    std::string unique_name = std::string("state#") + std::to_string(count);
    push_path(unique_name);

    // Visit the init expression to get its TypedValue.
    TypedValue init_tv = visit(init_node);
    if (init_tv.error) {
        pop_path();
        return TypedValue::error_val();
    }

    // Phase 4a: record-valued cells fan out per field.
    if (init_tv.type == ValueType::Record) {
        TypedValue result = handle_state_record_init(node, n, init_tv);
        pop_path();
        return result;
    }

    if (init_tv.type != ValueType::Signal && init_tv.type != ValueType::Number) {
        error("E122",
              "state() initial value must be a number, signal, or record",
              n.location);
        pop_path();
        return TypedValue::error_val();
    }

    // Scalar cell: state_id is the parent path's hash. Unchanged from Phase 3.
    std::uint32_t state_id = compute_state_id();
    std::uint16_t out = buffers_.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        pop_path();
        return TypedValue::error_val();
    }

    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::STATE_OP;
    inst.rate = 0;  // init mode
    inst.out_buffer = out;
    inst.inputs[0] = init_tv.buffer;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
    inst.state_id = state_id;
    emit(inst);

    pop_path();
    return cache_and_return(node, TypedValue::state_cell(state_id, out));
}

TypedValue CodeGenerator::handle_state_record_init(NodeIndex node, const Node& n,
                                                    const TypedValue& init_tv) {
    // Validate every init field is Number or Signal. CellState holds a single
    // float per slot; nested records require a future "record cells holding
    // record cells" facility deferred per PRD §10.4 (E204 for nested-field
    // write is the analogous restriction in Phase 4b).
    for (const auto& [name, field_tv] : init_tv.record->fields) {
        if (field_tv.type == ValueType::Record) {
            error("E122",
                  "state() record field '" + name +
                  "' must be a number or signal (nested record cells deferred)",
                  n.location);
            return TypedValue::error_val();
        }
        if (field_tv.type != ValueType::Signal && field_tv.type != ValueType::Number) {
            error("E122",
                  "state() record field '" + name +
                  "' must be a number or signal, got " +
                  value_type_name(field_tv.type),
                  n.location);
            return TypedValue::error_val();
        }
    }

    // Fan out in sorted-name order so codegen is deterministic across runs.
    std::unordered_map<std::string, TypedValue> sub_field_cells;
    std::uint16_t first_buf = BufferAllocator::BUFFER_UNUSED;

    for (const auto& field_name : sorted_field_names(*init_tv.record)) {
        const TypedValue& field_tv = init_tv.record->fields.at(field_name);

        push_path(field_name);
        std::uint32_t field_state_id = compute_state_id();

        std::uint16_t out = buffers_.allocate();
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            pop_path();
            return TypedValue::error_val();
        }

        cedar::Instruction inst{};
        inst.opcode = cedar::Opcode::STATE_OP;
        inst.rate = 0;  // init mode
        inst.out_buffer = out;
        inst.inputs[0] = field_tv.buffer;
        inst.inputs[1] = 0xFFFF;
        inst.inputs[2] = 0xFFFF;
        inst.inputs[3] = 0xFFFF;
        inst.inputs[4] = 0xFFFF;
        inst.state_id = field_state_id;
        emit(inst);

        sub_field_cells.emplace(field_name, TypedValue::state_cell(field_state_id, out));
        if (first_buf == BufferAllocator::BUFFER_UNUSED) first_buf = out;

        pop_path();
    }

    // Build the record-shaped StateCell. The empty-record case (state({}))
    // falls through to here with sub_field_cells empty and first_buf still
    // BUFFER_UNUSED — that's intentional and uniform.
    TypedValue tv;
    tv.type = ValueType::StateCell;
    tv.cell_state_id = RECORD_CELL_SENTINEL_ID;
    tv.buffer = first_buf;
    tv.record = std::make_shared<RecordPayload>();
    tv.record->fields = std::move(sub_field_cells);
    return cache_and_return(node, tv);
}

TypedValue CodeGenerator::handle_get_call(NodeIndex node, const Node& n) {
    NodeIndex arg = n.first_child;
    if (arg == NULL_NODE) {
        error("E120", "get() requires exactly 1 argument (a state cell)", n.location);
        return TypedValue::error_val();
    }
    NodeIndex cell_node = unwrap_argument(ast_->arena, arg);
    TypedValue cell_tv = visit(cell_node);
    if (cell_tv.error) return TypedValue::error_val();
    if (cell_tv.type != ValueType::StateCell) {
        error("E122",
              std::string("get() requires a state cell, got ") +
              value_type_name(cell_tv.type),
              n.location);
        return TypedValue::error_val();
    }

    // Phase 4a: record-valued cells return a fresh Record TypedValue.
    if (cell_tv.record) {
        return handle_get_record_cell(node, n, cell_tv);
    }

    std::uint16_t out = buffers_.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::error_val();
    }

    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::STATE_OP;
    inst.rate = 1;  // load mode
    inst.out_buffer = out;
    inst.inputs[0] = 0xFFFF;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
    inst.state_id = cell_tv.cell_state_id;
    emit(inst);

    return cache_and_return(node, TypedValue::signal(out));
}

TypedValue CodeGenerator::handle_get_record_cell(NodeIndex node, const Node& n,
                                                   const TypedValue& cell_tv) {
    std::unordered_map<std::string, TypedValue> result_fields;
    std::uint16_t first_buf = BufferAllocator::BUFFER_UNUSED;

    for (const auto& field_name : sorted_field_names(*cell_tv.record)) {
        const TypedValue& sub_cell = cell_tv.record->fields.at(field_name);

        std::uint16_t out = buffers_.allocate();
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::error_val();
        }

        cedar::Instruction inst{};
        inst.opcode = cedar::Opcode::STATE_OP;
        inst.rate = 1;  // load mode
        inst.out_buffer = out;
        inst.inputs[0] = 0xFFFF;
        inst.inputs[1] = 0xFFFF;
        inst.inputs[2] = 0xFFFF;
        inst.inputs[3] = 0xFFFF;
        inst.inputs[4] = 0xFFFF;
        inst.state_id = sub_cell.cell_state_id;
        emit(inst);

        result_fields.emplace(field_name, TypedValue::signal(out));
        if (first_buf == BufferAllocator::BUFFER_UNUSED) first_buf = out;
    }

    return cache_and_return(node,
                             TypedValue::make_record(std::move(result_fields), first_buf));
}

TypedValue CodeGenerator::handle_set_call(NodeIndex node, const Node& n) {
    NodeIndex arg1 = n.first_child;
    if (arg1 == NULL_NODE) {
        error("E120", "set() requires 2 arguments (cell, value)", n.location);
        return TypedValue::error_val();
    }
    NodeIndex arg2 = ast_->arena[arg1].next_sibling;
    if (arg2 == NULL_NODE) {
        error("E120", "set() requires 2 arguments (cell, value)", n.location);
        return TypedValue::error_val();
    }

    NodeIndex cell_node = unwrap_argument(ast_->arena, arg1);
    NodeIndex value_node = unwrap_argument(ast_->arena, arg2);

    TypedValue cell_tv = visit(cell_node);
    if (cell_tv.error) return TypedValue::error_val();
    if (cell_tv.type != ValueType::StateCell) {
        error("E122",
              std::string("set() first argument must be a state cell, got ") +
              value_type_name(cell_tv.type),
              n.location);
        return TypedValue::error_val();
    }

    TypedValue value_tv = visit(value_node);
    if (value_tv.error) return TypedValue::error_val();

    // Phase 4a: record-valued cells dispatch to a fan-out path with shape
    // validation. A scalar cell rejects records, and vice versa, so the user
    // gets a clear error rather than a silent partial-write.
    if (cell_tv.record) {
        if (value_tv.type != ValueType::Record) {
            error("E122",
                  std::string("set() on record cell requires a record, got ") +
                  value_type_name(value_tv.type),
                  n.location);
            return TypedValue::error_val();
        }
        if (!record_shapes_match(*cell_tv.record, *value_tv.record)) {
            error("E189",
                  "State cell shape mismatch — expected " +
                  shape_csv(*cell_tv.record) + ", got " +
                  shape_csv(*value_tv.record),
                  n.location);
            return TypedValue::error_val();
        }
        return handle_set_record_cell(node, n, cell_tv, value_tv);
    }

    if (value_tv.type == ValueType::Record) {
        error("E122",
              "set() on scalar cell requires a number or signal, got Record",
              n.location);
        return TypedValue::error_val();
    }
    if (value_tv.type != ValueType::Signal && value_tv.type != ValueType::Number) {
        error("E122", "set() value must be a number or signal", n.location);
        return TypedValue::error_val();
    }

    std::uint16_t out = buffers_.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::error_val();
    }

    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::STATE_OP;
    inst.rate = 2;  // store mode
    inst.out_buffer = out;
    inst.inputs[0] = value_tv.buffer;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
    inst.state_id = cell_tv.cell_state_id;
    emit(inst);

    // Returning a Signal lets users chain set() in expression position:
    //   arr[s.set(s.get() + 1)] — advance and use the new index in one shot.
    return cache_and_return(node, TypedValue::signal(out));
}

TypedValue CodeGenerator::handle_field_assignment(NodeIndex node, const Node& n) {
    // Phase 4b write sugar: `receiver.field = value` lowers to the moral
    // equivalent of `set(receiver, {..get(receiver), field: value})`. Since
    // record cells are already fanned out into per-field scalar slots, the
    // observable effect is a single store on the targeted slot — emit one
    // STATE_OP rate=2 directly instead of the literal get+spread+set fan-out.
    const auto& fa = n.as_field_assignment();
    const std::string& field_name = fa.field_name;

    NodeIndex receiver_idx = n.first_child;
    if (receiver_idx == NULL_NODE) {
        error("E104", "Invalid field assignment", n.location);
        return TypedValue::error_val();
    }
    NodeIndex value_idx = ast_->arena[receiver_idx].next_sibling;
    if (value_idx == NULL_NODE) {
        error("E104", "Field assignment missing value", n.location);
        return TypedValue::error_val();
    }

    // Reject nested-field writes (`cell.outer.inner = v`) up front. The
    // analyzer hint is more useful than letting the receiver evaluate to a
    // Signal and bouncing off "field assignment on non-cell".
    if (ast_->arena[receiver_idx].type == NodeType::FieldAccess) {
        error("E204",
              "Nested field assignment is not supported in v1; rewrite as "
              "`set(cell, {..get(cell), inner: {..get(cell).inner, " +
              field_name + ": v}})`.",
              n.location);
        return TypedValue::error_val();
    }

    TypedValue cell_tv = visit(receiver_idx);
    if (cell_tv.error) return TypedValue::error_val();

    if (cell_tv.type == ValueType::Record) {
        error("E150",
              "Cannot assign to field '" + field_name +
              "' of immutable value record. Declare with `state({...})` to "
              "allow mutation.",
              n.location);
        return TypedValue::error_val();
    }
    if (cell_tv.type != ValueType::StateCell) {
        error("E150",
              std::string("Cannot assign to field on ") +
              value_type_name(cell_tv.type) +
              " value. Field assignment requires a state cell.",
              n.location);
        return TypedValue::error_val();
    }
    if (!cell_tv.record) {
        error("E135",
              "Cannot assign field '" + field_name +
              "' on scalar state cell. Use `set(cell, value)` for scalar cells.",
              n.location);
        return TypedValue::error_val();
    }

    auto sub_it = cell_tv.record->fields.find(field_name);
    if (sub_it == cell_tv.record->fields.end()) {
        std::string available;
        bool first = true;
        for (const auto& [name, _] : cell_tv.record->fields) {
            if (!first) available += ", ";
            available += name;
            first = false;
        }
        error("E136",
              "Unknown field '" + field_name +
              "' on state cell" +
              (available.empty() ? "" : ". Available: " + available),
              n.location);
        return TypedValue::error_val();
    }
    const TypedValue& sub_cell = sub_it->second;

    TypedValue value_tv = visit(value_idx);
    if (value_tv.error) return TypedValue::error_val();

    // Per-field cells store a single float at runtime; reject Record (the
    // user is reaching for the deferred nested-cell facility) and any other
    // non-scalar type.
    if (value_tv.type == ValueType::Record) {
        error("E204",
              "Nested field assignment is not supported in v1; assigning a "
              "record to field '" + field_name + "' would require nested "
              "record cells. Rewrite as per-field assignments instead.",
              n.location);
        return TypedValue::error_val();
    }
    if (value_tv.type != ValueType::Signal && value_tv.type != ValueType::Number) {
        error("E122",
              "Field assignment value must be a number or signal, got " +
              std::string(value_type_name(value_tv.type)),
              n.location);
        return TypedValue::error_val();
    }

    std::uint16_t out = buffers_.allocate();
    if (out == BufferAllocator::BUFFER_UNUSED) {
        error("E101", "Buffer pool exhausted", n.location);
        return TypedValue::error_val();
    }

    cedar::Instruction inst{};
    inst.opcode = cedar::Opcode::STATE_OP;
    inst.rate = 2;  // store mode
    inst.out_buffer = out;
    inst.inputs[0] = value_tv.buffer;
    inst.inputs[1] = 0xFFFF;
    inst.inputs[2] = 0xFFFF;
    inst.inputs[3] = 0xFFFF;
    inst.inputs[4] = 0xFFFF;
    inst.state_id = sub_cell.cell_state_id;
    emit(inst);

    // The desugaring of `cell.x = expr` is conceptually
    //   `set(cell, {..get(cell), x: expr})`
    // which evaluates to a Record. Statement context discards the result, so
    // void_val matches both the typical use and the void of plain assignment.
    return cache_and_return(node, TypedValue::void_val());
}

TypedValue CodeGenerator::handle_set_record_cell(NodeIndex node, const Node& n,
                                                   const TypedValue& cell_tv,
                                                   const TypedValue& value_tv) {
    std::unordered_map<std::string, TypedValue> result_fields;
    std::uint16_t first_buf = BufferAllocator::BUFFER_UNUSED;

    for (const auto& field_name : sorted_field_names(*cell_tv.record)) {
        const TypedValue& sub_cell = cell_tv.record->fields.at(field_name);
        // record_shapes_match guarantees the field exists in value_tv.
        const TypedValue& field_value = value_tv.record->fields.at(field_name);

        // Each cell field stores a single float at runtime (CellState), so the
        // value's field must be Number/Signal. A nested record here would mean
        // the user is trying to nest cells, which is deferred (see E204 in
        // Phase 4b).
        if (field_value.type != ValueType::Signal &&
            field_value.type != ValueType::Number) {
            error("E122",
                  "set() record field '" + field_name +
                  "' value must be a number or signal, got " +
                  value_type_name(field_value.type),
                  n.location);
            return TypedValue::error_val();
        }

        std::uint16_t out = buffers_.allocate();
        if (out == BufferAllocator::BUFFER_UNUSED) {
            error("E101", "Buffer pool exhausted", n.location);
            return TypedValue::error_val();
        }

        cedar::Instruction inst{};
        inst.opcode = cedar::Opcode::STATE_OP;
        inst.rate = 2;  // store mode
        inst.out_buffer = out;
        inst.inputs[0] = field_value.buffer;
        inst.inputs[1] = 0xFFFF;
        inst.inputs[2] = 0xFFFF;
        inst.inputs[3] = 0xFFFF;
        inst.inputs[4] = 0xFFFF;
        inst.state_id = sub_cell.cell_state_id;
        emit(inst);

        result_fields.emplace(field_name, TypedValue::signal(out));
        if (first_buf == BufferAllocator::BUFFER_UNUSED) first_buf = out;
    }

    return cache_and_return(node,
                             TypedValue::make_record(std::move(result_fields), first_buf));
}

}  // namespace akkado
