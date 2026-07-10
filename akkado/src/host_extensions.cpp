#ifdef CEDAR_HOST_EXTENSIONS

#include "akkado/host_extensions.hpp"

#include <cassert>
#include <cmath>
#include <deque>
#include <unordered_map>

namespace akkado {

namespace {

// Owned backing storage for host descriptors. A deque never invalidates
// references to existing elements on push_back, so the string_views a
// synthesized BuiltinInfo hands to the compiler stay valid for the process
// lifetime (prd-host-extension-api §13-Q2).
struct HostEntry {
    std::string name;
    std::array<std::string, 5> param_names;
    std::string description;
    BuiltinInfo info{};
    std::uint8_t host_index = 0;
    bool is_node = false;
    bool accepts_events = false;
};

struct HostVarEntry {
    std::string name;
    std::string getter_name;
    std::string setter_name;
    std::string env_key;
    BuiltinVarDef def{};
};

struct Registry {
    std::deque<HostEntry> functions;
    std::deque<HostVarEntry> variables;
    std::unordered_map<std::string_view, const HostEntry*> by_name;
    std::unordered_map<std::string_view, const HostVarEntry*> vars_by_name;
    std::uint16_t next_index = 0;  // 16-bit so exhaustion at 256 is detectable
    bool frozen = false;
};

Registry& registry() {
    static Registry r;
    return r;
}

// A host name may never shadow a core builtin, a core alias, a core variable,
// or an earlier host registration.
bool name_is_taken(std::string_view name) {
    auto& r = registry();
    return BUILTIN_FUNCTIONS.count(name) != 0 || BUILTIN_ALIASES.count(name) != 0
           || BUILTIN_VARIABLES.count(name) != 0 || r.by_name.count(name) != 0
           || r.vars_by_name.count(name) != 0;
}

// Build a BuiltinInfo whose string_views point into the entry's owned strings.
void synthesize_builtin_info(HostEntry& e, const HostFunctionDesc& desc, bool host_op) {
    BuiltinInfo& info = e.info;
    info.opcode = host_op ? cedar::Opcode::HOST_OP : desc.core_opcode;
    info.input_count = desc.input_count;
    info.optional_count = desc.optional_count;
    info.requires_state = desc.requires_state;
    info.description = e.description;
    info.output_channels = desc.output_channels;
    info.stereo_native = desc.stereo_native;
    // Core codegen copies BuiltinInfo::inst_rate straight into inst.rate, which
    // is precisely the dispatch index HOST_OP needs — no new codegen path.
    info.inst_rate = host_op ? e.host_index : 0;

    for (std::size_t i = 0; i < 5; ++i) {
        info.param_names[i] = e.param_names[i];
        info.param_types[i] = desc.param_types[i];
    }
    for (std::size_t i = 0; i < 5 && i < MAX_BUILTIN_DEFAULTS; ++i) {
        info.defaults[i] = desc.defaults[i];
    }
}

bool validate(const HostFunctionDesc& desc, cedar::HostOpFn impl_fn) {
    if (desc.name.empty()) return false;
    // input_count is the *required* arity, optional_count the additional
    // defaulted slots; together they must fit the five instruction inputs.
    // (Core `lp` is 2 + 3, so this is not `optional_count <= input_count`.)
    if (desc.input_count + desc.optional_count > 5) return false;
    // Null impl means "alias an existing core opcode" — that opcode must be real.
    if (impl_fn == nullptr && desc.core_opcode == cedar::Opcode::INVALID) return false;
    // A real host op must not also claim a core opcode.
    if (impl_fn != nullptr && desc.core_opcode != cedar::Opcode::INVALID) return false;
    return true;
}

bool register_function_impl(const HostFunctionDesc& desc, cedar::HostOpFn impl_fn,
                            bool is_node, bool accepts_events) {
    auto& r = registry();
    if (r.frozen) {
        assert(false && "host extensions registered after audio started");
        return false;
    }
    if (!validate(desc, impl_fn)) return false;
    if (name_is_taken(desc.name)) return false;

    const bool host_op = (impl_fn != nullptr);
    if (host_op && r.next_index >= cedar::MAX_HOST_OPS) return false;

    HostEntry& e = r.functions.emplace_back();
    e.name = desc.name;
    e.param_names = desc.param_names;
    e.description = desc.description;
    e.is_node = is_node;
    e.accepts_events = accepts_events;
    e.host_index = host_op ? static_cast<std::uint8_t>(r.next_index) : 0;

    if (host_op) {
        if (!cedar::HostOpRegistry::instance().register_op(e.host_index, impl_fn,
                                                           desc.state_bytes)) {
            r.functions.pop_back();
            return false;
        }
        ++r.next_index;
    }

    synthesize_builtin_info(e, desc, host_op);
    r.by_name.emplace(std::string_view(e.name), &e);
    return true;
}

}  // namespace

bool register_host_function(const HostFunctionDesc& desc, cedar::HostOpFn impl_fn) {
    return register_function_impl(desc, impl_fn, /*is_node=*/false, /*accepts_events=*/false);
}

bool register_host_node(const HostNodeDesc& desc, cedar::HostOpFn impl_fn) {
    // A node is always a real host op with per-call-site state: the state_id is
    // what the host pools its instance by.
    if (impl_fn == nullptr) return false;
    // No consumer for event-stream inputs yet — see HostNodeDesc::accepts_events.
    if (desc.accepts_events) return false;
    // Exactly one String slot names the instance; codegen records it into the
    // manifest instead of wiring it to an input buffer.
    int string_slots = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        if (desc.fn.param_types[i] == ParamValueType::String) ++string_slots;
    }
    if (string_slots != 1) return false;

    HostFunctionDesc fn = desc.fn;
    fn.requires_state = true;
    return register_function_impl(fn, impl_fn, /*is_node=*/true, desc.accepts_events);
}

bool register_host_variable(const HostVariableDesc& desc) {
    auto& r = registry();
    if (r.frozen) {
        assert(false && "host extensions registered after audio started");
        return false;
    }
    if (desc.name.empty()) return false;
    if (name_is_taken(desc.name)) return false;
    // Audio-rate host variables are Phase 4; reject rather than silently
    // degrading them to control rate.
    if (desc.rate != HostVarRate::Control) return false;

    HostVarEntry& e = r.variables.emplace_back();
    e.name = desc.name;
    e.getter_name = "get_host_" + desc.name;
    e.setter_name = "set_host_" + desc.name;
    e.env_key = "__host_" + desc.name;
    e.def = BuiltinVarDef{e.getter_name, e.setter_name, e.env_key,
                          desc.default_val, desc.min, desc.max};
    r.vars_by_name.emplace(std::string_view(e.name), &e);
    return true;
}

const BuiltinInfo* lookup_host_builtin(std::string_view name) {
    auto& r = registry();
    auto it = r.by_name.find(name);
    return it == r.by_name.end() ? nullptr : &it->second->info;
}

const BuiltinVarDef* lookup_host_variable(std::string_view name) {
    auto& r = registry();
    auto it = r.vars_by_name.find(name);
    return it == r.vars_by_name.end() ? nullptr : &it->second->def;
}

std::string_view host_variable_env_key(std::string_view name) {
    auto& r = registry();
    auto it = r.vars_by_name.find(name);
    return it == r.vars_by_name.end() ? std::string_view{} : it->second->def.env_key;
}

bool is_host_node(std::string_view name) {
    auto& r = registry();
    auto it = r.by_name.find(name);
    return it != r.by_name.end() && it->second->is_node;
}

int host_node_string_slot(std::string_view name) {
    auto& r = registry();
    auto it = r.by_name.find(name);
    if (it == r.by_name.end() || !it->second->is_node) return -1;
    const BuiltinInfo& info = it->second->info;
    for (int i = 0; i < 5; ++i) {
        if (info.param_types[static_cast<std::size_t>(i)] == ParamValueType::String) return i;
    }
    return -1;
}

std::vector<std::pair<std::string_view, const BuiltinInfo*>> host_functions() {
    auto& r = registry();
    std::vector<std::pair<std::string_view, const BuiltinInfo*>> out;
    out.reserve(r.functions.size());
    for (const auto& e : r.functions) {
        out.emplace_back(std::string_view(e.name), &e.info);
    }
    return out;
}

void reset_host_extensions_for_testing() {
    auto& r = registry();
    r.by_name.clear();
    r.vars_by_name.clear();
    r.functions.clear();
    r.variables.clear();
    r.next_index = 0;
    r.frozen = false;
    cedar::HostOpRegistry::instance().reset_for_testing();
}

}  // namespace akkado

#endif  // CEDAR_HOST_EXTENSIONS
