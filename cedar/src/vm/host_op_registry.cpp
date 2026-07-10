#include "cedar/vm/host_op_registry.hpp"

namespace cedar {

HostOpRegistry& HostOpRegistry::instance() {
    static HostOpRegistry registry;
    return registry;
}

bool HostOpRegistry::register_op(std::uint8_t index, HostOpFn fn, std::uint32_t state_bytes) {
    if (fn == nullptr) return false;
    if (table_[index] != nullptr) return false;  // collisions are rejected, never overwritten
    table_[index] = fn;
    state_bytes_[index] = state_bytes;
    return true;
}

void HostOpRegistry::reset_for_testing() {
    table_.fill(nullptr);
    state_bytes_.fill(0);
}

}  // namespace cedar
