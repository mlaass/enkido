// Tests for the host extension API (prd-host-extension-api §10, Phases 1-3).
// Compiled only when CEDAR_HOST_EXTENSIONS is on.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <akkado/akkado.hpp>
#include <akkado/host_extensions.hpp>
#include <akkado/builtins.hpp>

#include <cedar/vm/instruction.hpp>
#include <cedar/vm/vm.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

std::vector<cedar::Instruction> get_instructions(const akkado::CompileResult& result) {
    std::vector<cedar::Instruction> insts(result.bytecode.size() / sizeof(cedar::Instruction));
    std::memcpy(insts.data(), result.bytecode.data(), result.bytecode.size());
    return insts;
}

const cedar::Instruction* find_op(const std::vector<cedar::Instruction>& insts, cedar::Opcode op) {
    for (const auto& i : insts) {
        if (i.opcode == op) return &i;
    }
    return nullptr;
}

bool has_error(const akkado::CompileResult& r) {
    for (const auto& d : r.diagnostics) {
        if (d.severity == akkado::Severity::Error) return true;
    }
    return false;
}

void noop_impl(cedar::ExecutionContext&, const cedar::Instruction&) {}

struct Guard {
    Guard() { akkado::reset_host_extensions_for_testing(); }
    ~Guard() { akkado::reset_host_extensions_for_testing(); }
};

akkado::HostFunctionDesc simple_desc(std::string name, std::uint8_t inputs = 1) {
    akkado::HostFunctionDesc d;
    d.name = std::move(name);
    d.input_count = inputs;
    for (std::uint8_t i = 0; i < inputs; ++i) {
        d.param_names[i] = "in" + std::to_string(i);
        d.param_types[i] = akkado::ParamValueType::Signal;
    }
    d.description = "test host function";
    return d;
}

}  // namespace

// ---------------------------------------------------------------------------
// Phase 1 — host variables (control-rate)
// ---------------------------------------------------------------------------

TEST_CASE("host variable resolves as an identifier and desugars to ENV_GET", "[host-extensions]") {
    Guard guard;
    akkado::HostVariableDesc v;
    v.name = "health";
    v.default_val = 0.5f;
    v.min = 0.0f;
    v.max = 1.0f;
    REQUIRE(akkado::register_host_variable(v));

    auto result = akkado::compile("out(lp(saw(110), 200 + health * 3000))");
    REQUIRE(result.success);
    REQUIRE_FALSE(has_error(result));

    // The identifier lowered to an env read, exactly like bpm/sr do.
    auto insts = get_instructions(result);
    REQUIRE(find_op(insts, cedar::Opcode::ENV_GET) != nullptr);
}

TEST_CASE("host variable is driven end-to-end via set_param", "[host-extensions]") {
    Guard guard;
    akkado::HostVariableDesc v;
    v.name = "health";
    v.default_val = 0.25f;
    REQUIRE(akkado::register_host_variable(v));

    auto result = akkado::compile("out(health)");
    REQUIRE(result.success);
    REQUIRE_FALSE(has_error(result));

    cedar::VM vm;
    vm.set_sample_rate(48000.0f);
    auto insts = get_instructions(result);
    REQUIRE(vm.load_program_immediate(insts));

    // The raw name is not the channel — the reserved env key is.
    const std::string key(akkado::host_variable_env_key("health"));
    REQUIRE(key == "__host_health");

    // Zero slew so the value settles within one block. Amplitudes stay small
    // because out() runs the master saturator — at 0.75 the output reads 0.625,
    // which is the limiter doing its job, not the variable failing to track.
    std::array<float, cedar::BLOCK_SIZE> left{}, right{};
    vm.set_param(key.c_str(), 0.10f, 0.0f);
    vm.process_block(left.data(), right.data());
    CHECK_THAT(left[cedar::BLOCK_SIZE - 1], WithinAbs(0.10f, 1e-3f));

    vm.set_param(key.c_str(), -0.05f, 0.0f);
    vm.process_block(left.data(), right.data());
    CHECK_THAT(left[cedar::BLOCK_SIZE - 1], WithinAbs(-0.05f, 1e-3f));
}

TEST_CASE("unregistered identifier is still an error", "[host-extensions]") {
    Guard guard;  // nothing registered
    auto result = akkado::compile("out(saw(110) * health)");
    REQUIRE(has_error(result));
}

TEST_CASE("host variable cannot shadow a core variable or builtin", "[host-extensions]") {
    Guard guard;
    akkado::HostVariableDesc v;
    v.name = "bpm";
    REQUIRE_FALSE(akkado::register_host_variable(v));

    v.name = "lp";  // core builtin function
    REQUIRE_FALSE(akkado::register_host_variable(v));

    v.name = "ok_name";
    REQUIRE(akkado::register_host_variable(v));
    REQUIRE_FALSE(akkado::register_host_variable(v));  // duplicate
}

TEST_CASE("audio-rate host variables are rejected until Phase 4", "[host-extensions]") {
    Guard guard;
    akkado::HostVariableDesc v;
    v.name = "cv_in";
    v.rate = akkado::HostVarRate::Audio;
    REQUIRE_FALSE(akkado::register_host_variable(v));
}

// ---------------------------------------------------------------------------
// Phase 2 — host functions over an existing core opcode (null impl)
// ---------------------------------------------------------------------------

TEST_CASE("null-impl host function emits the mapped core opcode", "[host-extensions]") {
    Guard guard;

    // Mirror `lp`'s real descriptor shape (2 required + 3 optional, stereo-native)
    // so the alias is a true rename rather than a differently-shaped builtin.
    akkado::HostFunctionDesc d;
    d.name = "mylowpass";
    d.core_opcode = cedar::Opcode::FILTER_SVF_LP;
    d.input_count = 2;
    d.optional_count = 3;
    d.param_names = {"in", "cut", "q", "dry", "wet"};
    d.defaults = {0.707f, 0.0f, 1.0f, NAN, NAN};
    d.requires_state = true;
    d.output_channels = akkado::ChannelCount::Stereo;
    d.stereo_native = true;
    d.description = "State-variable lowpass filter";
    REQUIRE(akkado::register_host_function(d, nullptr));

    auto aliased = akkado::compile("out(mylowpass(saw(110), 800))");
    REQUIRE(aliased.success);
    REQUIRE_FALSE(has_error(aliased));

    auto core = akkado::compile("out(lp(saw(110), 800))");
    REQUIRE(core.success);

    // Same opcode stream: the alias is indistinguishable from the core builtin.
    auto a = get_instructions(aliased);
    auto c = get_instructions(core);
    REQUIRE(a.size() == c.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].opcode == c[i].opcode);
    }
    REQUIRE(find_op(a, cedar::Opcode::HOST_OP) == nullptr);
}

TEST_CASE("host function name collisions are rejected", "[host-extensions]") {
    Guard guard;
    auto d = simple_desc("lp", 2);  // core builtin
    d.core_opcode = cedar::Opcode::FILTER_SVF_LP;
    REQUIRE_FALSE(akkado::register_host_function(d, nullptr));

    auto ok = simple_desc("hwcrush");
    REQUIRE(akkado::register_host_function(ok, &noop_impl));
    REQUIRE_FALSE(akkado::register_host_function(ok, &noop_impl));  // duplicate name

    // An alias over an alias name is rejected too.
    auto over_alias = simple_desc("limit", 1);  // BUILTIN_ALIASES entry
    over_alias.core_opcode = cedar::Opcode::DYNAMICS_LIMITER;
    REQUIRE_FALSE(akkado::register_host_function(over_alias, nullptr));
}

TEST_CASE("malformed host function descriptors are rejected", "[host-extensions]") {
    Guard guard;

    auto unnamed = simple_desc("");
    REQUIRE_FALSE(akkado::register_host_function(unnamed, &noop_impl));

    // Null impl with no core opcode: nothing to emit.
    auto nothing = simple_desc("nothing");
    REQUIRE_FALSE(akkado::register_host_function(nothing, nullptr));

    // A real host op must not also claim a core opcode.
    auto both = simple_desc("both");
    both.core_opcode = cedar::Opcode::FILTER_SVF_LP;
    REQUIRE_FALSE(akkado::register_host_function(both, &noop_impl));

    auto too_many = simple_desc("toomany");
    too_many.input_count = 6;
    REQUIRE_FALSE(akkado::register_host_function(too_many, &noop_impl));
}

// ---------------------------------------------------------------------------
// Phase 3 — genuine host opcodes
// ---------------------------------------------------------------------------

TEST_CASE("host opcode emits HOST_OP with rate = dispatch index", "[host-extensions]") {
    Guard guard;

    auto first = simple_desc("hwcrush");
    first.requires_state = true;
    first.state_bytes = 64;
    REQUIRE(akkado::register_host_function(first, &noop_impl));

    auto second = simple_desc("hwfold");
    REQUIRE(akkado::register_host_function(second, &noop_impl));

    auto r0 = akkado::compile("out(hwcrush(saw(220)))");
    REQUIRE(r0.success);
    REQUIRE_FALSE(has_error(r0));
    const auto* i0 = find_op(get_instructions(r0), cedar::Opcode::HOST_OP);
    REQUIRE(i0 != nullptr);
    CHECK(i0->rate == 0);            // first registration → index 0
    CHECK(i0->state_id != 0);        // requires_state → semantic ID allocated

    auto r1 = akkado::compile("out(hwfold(saw(220)))");
    REQUIRE(r1.success);
    const auto* i1 = find_op(get_instructions(r1), cedar::Opcode::HOST_OP);
    REQUIRE(i1 != nullptr);
    CHECK(i1->rate == 1);            // second registration → index 1
}

TEST_CASE("host op state_id is stable across recompiles of the same source", "[host-extensions]") {
    Guard guard;
    auto d = simple_desc("hwcrush");
    d.requires_state = true;
    d.state_bytes = 64;
    REQUIRE(akkado::register_host_function(d, &noop_impl));

    const char* src = "out(hwcrush(saw(220)))";
    const auto* a = find_op(get_instructions(akkado::compile(src)), cedar::Opcode::HOST_OP);
    const auto* b = find_op(get_instructions(akkado::compile(src)), cedar::Opcode::HOST_OP);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    // This is what lets the studio's pool rebind an instance across a hot-swap.
    CHECK(a->state_id == b->state_id);
}

// ---------------------------------------------------------------------------
// register_host_node — the studio's plugin() seam (prd-studio-plugin-hosting OQ12)
// ---------------------------------------------------------------------------

namespace {

akkado::HostNodeDesc plugin_node_desc(std::string name = "plugin") {
    akkado::HostNodeDesc d;
    d.fn.name = std::move(name);
    d.fn.input_count = 1;   // the string-literal instance name
    d.fn.optional_count = 1; // one signal slot, e.g. the effect input
    d.fn.param_names = {"name", "in"};
    d.fn.param_types[0] = akkado::ParamValueType::String;
    d.fn.param_types[1] = akkado::ParamValueType::Signal;
    d.fn.defaults = {NAN, 0.0f};
    d.fn.description = "host a third-party plugin";
    return d;
}

}  // namespace

TEST_CASE("host node records its call site in the manifest", "[host-extensions]") {
    Guard guard;
    REQUIRE(akkado::register_host_node(plugin_node_desc(), &noop_impl));

    auto r = akkado::compile("out(plugin(\"Diva\"))");
    REQUIRE(r.success);
    REQUIRE_FALSE(has_error(r));

    REQUIRE(r.required_host_nodes.size() == 1);
    const auto& entry = r.required_host_nodes[0];
    CHECK(entry.name == "Diva");
    CHECK(entry.host_index == 0);
    CHECK(entry.state_id != 0);

    // The manifest state_id is the one the emitted instruction carries, which is
    // what lets the host bind a pooled instance to this exact call site.
    const auto* inst = find_op(get_instructions(r), cedar::Opcode::HOST_OP);
    REQUIRE(inst != nullptr);
    CHECK(inst->state_id == entry.state_id);
}

TEST_CASE("two host node call sites get distinct semantic IDs", "[host-extensions]") {
    Guard guard;
    REQUIRE(akkado::register_host_node(plugin_node_desc(), &noop_impl));

    auto r = akkado::compile("out(plugin(\"Diva\") + plugin(\"Surge\"))");
    REQUIRE(r.success);
    REQUIRE(r.required_host_nodes.size() == 2);
    CHECK(r.required_host_nodes[0].state_id != r.required_host_nodes[1].state_id);

    std::vector<std::string> names{r.required_host_nodes[0].name, r.required_host_nodes[1].name};
    CHECK(std::find(names.begin(), names.end(), "Diva") != names.end());
    CHECK(std::find(names.begin(), names.end(), "Surge") != names.end());
}

TEST_CASE("host node name must be a string literal", "[host-extensions]") {
    Guard guard;
    REQUIRE(akkado::register_host_node(plugin_node_desc(), &noop_impl));

    // A non-literal name cannot be resolved at compile time — the host would
    // have nothing to pre-bind.
    auto r = akkado::compile("out(plugin(440))");
    CHECK(has_error(r));
}

TEST_CASE("host node registration rejects unwired event inputs", "[host-extensions]") {
    Guard guard;
    auto d = plugin_node_desc();
    d.accepts_events = true;  // no consumer yet — must not silently no-op
    REQUIRE_FALSE(akkado::register_host_node(d, &noop_impl));
}

TEST_CASE("host node requires exactly one string slot and a real impl", "[host-extensions]") {
    Guard guard;

    auto no_impl = plugin_node_desc();
    REQUIRE_FALSE(akkado::register_host_node(no_impl, nullptr));

    auto no_string = plugin_node_desc("nostring");
    no_string.fn.param_types[0] = akkado::ParamValueType::Signal;
    REQUIRE_FALSE(akkado::register_host_node(no_string, &noop_impl));

    auto two_strings = plugin_node_desc("twostrings");
    two_strings.fn.param_types[1] = akkado::ParamValueType::String;
    REQUIRE_FALSE(akkado::register_host_node(two_strings, &noop_impl));
}

TEST_CASE("host node state_id survives a recompile of the same source", "[host-extensions]") {
    Guard guard;
    REQUIRE(akkado::register_host_node(plugin_node_desc(), &noop_impl));

    const char* src = "out(plugin(\"Diva\"))";
    auto a = akkado::compile(src);
    auto b = akkado::compile(src);
    REQUIRE(a.required_host_nodes.size() == 1);
    REQUIRE(b.required_host_nodes.size() == 1);
    // The pooling contract: same semantic path → same key → no reload.
    CHECK(a.required_host_nodes[0].state_id == b.required_host_nodes[0].state_id);
}

TEST_CASE("a program using no host ops is unaffected by registration", "[host-extensions]") {
    const char* src = "out(lp(saw(110), 800))";

    akkado::reset_host_extensions_for_testing();
    auto before = akkado::compile(src);
    REQUIRE(before.success);

    Guard guard;
    auto d = simple_desc("hwcrush");
    REQUIRE(akkado::register_host_function(d, &noop_impl));
    auto after = akkado::compile(src);
    REQUIRE(after.success);

    // Byte-identical bytecode: registering a host op perturbs nothing.
    REQUIRE(before.bytecode == after.bytecode);
}
