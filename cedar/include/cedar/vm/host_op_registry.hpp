#pragma once

// Host opcode registry (prd-host-extension-api §6.4).
//
// An embedder registers up to 256 opcode implementations at init. The VM
// dispatches `HOST_OP` through `table[inst.rate]`, so host extensibility costs
// exactly one enum slot no matter how many host ops exist.
//
// Registration happens at host init and is read-only afterward: both the
// compile thread and the audio thread only read the table, so no locking is
// needed. `impl_fn` runs on the audio thread under the usual contract — no
// allocation, no locks, no blocking.

#include <array>
#include <cstddef>
#include <cstdint>

namespace cedar {

struct ExecutionContext;
struct Instruction;

/// Audio-thread implementation of a host opcode. Same signature core opcodes get.
using HostOpFn = void (*)(ExecutionContext&, const Instruction&);

/// Maximum number of distinct host opcodes, bounded by `Instruction::rate` (uint8).
inline constexpr std::size_t MAX_HOST_OPS = 256;

class HostOpRegistry {
public:
    static HostOpRegistry& instance();

    /// Bind `index` to `fn`. `state_bytes` is the per-instance arena region the
    /// loader reserves for a stateful host op (0 = stateless).
    /// Returns false if the index is already bound or `fn` is null.
    bool register_op(std::uint8_t index, HostOpFn fn, std::uint32_t state_bytes = 0);

    [[nodiscard]] HostOpFn get(std::uint8_t index) const { return table_[index]; }
    [[nodiscard]] std::uint32_t state_bytes(std::uint8_t index) const { return state_bytes_[index]; }
    [[nodiscard]] bool is_bound(std::uint8_t index) const { return table_[index] != nullptr; }

    /// Test-only. Registration is otherwise immutable for the process lifetime.
    void reset_for_testing();

private:
    HostOpRegistry() = default;

    std::array<HostOpFn, MAX_HOST_OPS> table_{};
    std::array<std::uint32_t, MAX_HOST_OPS> state_bytes_{};
};

}  // namespace cedar
