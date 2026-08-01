#include "akkado/named_args.hpp"

#include <set>

namespace akkado {

NamedArgSlots assign_named_arg_slots(
    const std::vector<NamedArgInput>& args,
    const std::string& func_name,
    const std::function<int(std::string_view, std::size_t)>& find_param,
    bool drop_unknown) {
    NamedArgSlots out;
    if (args.empty()) return out;

    std::vector<int> target(args.size(), -1);
    bool seen_named = false;
    std::set<std::string> used_params;

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i].name.has_value()) {
            out.has_named = true;
            seen_named = true;
            const std::string& name = *args[i].name;

            if (!used_params.insert(name).second) {
                out.ok = false;
                out.code = "E010";
                out.message = "Duplicate named argument '" + name +
                              "' in call to '" + func_name + "'";
                out.error_loc = args[i].loc;
                return out;
            }

            int param_idx = find_param(name, i);
            if (param_idx == kNamedArgAbort) {
                out.ok = false;  // callback already diagnosed
                return out;
            }
            if (param_idx < 0) {
                if (drop_unknown) {
                    out.dropped.push_back(i);
                    continue;  // target stays -1 → dropped
                }
                out.ok = false;
                out.code = "E011";
                out.message = "Unknown parameter '" + name +
                              "' for function '" + func_name + "'";
                out.error_loc = args[i].loc;
                return out;
            }
            target[i] = param_idx;
        } else {
            if (seen_named) {
                out.ok = false;
                out.code = "E009";
                out.message = "Positional argument cannot follow named "
                              "argument in call to '" + func_name + "'";
                out.error_loc = args[i].loc;
                return out;
            }
            target[i] = static_cast<int>(i);
        }
    }

    if (!out.has_named) return out;  // already positional — nothing to do

    // A named arg must not collide with a positional one filling the
    // same slot.
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i].name.has_value()) continue;
        for (std::size_t j = 0; j < args.size(); ++j) {
            if (args[j].name.has_value() && target[j] >= 0 &&
                target[j] == static_cast<int>(i)) {
                out.ok = false;
                out.code = "E012";
                out.message = "Parameter '" + *args[j].name +
                              "' at position " + std::to_string(i) +
                              " conflicts with positional argument in "
                              "call to '" + func_name + "'";
                out.error_loc = args[i].loc;
                return out;
            }
        }
    }

    // Build the slot → source-arg table.
    int max_pos = -1;
    for (int t : target) {
        if (t > max_pos) max_pos = t;
    }
    if (max_pos < 0) return out;  // every argument dropped

    out.slot_source.assign(static_cast<std::size_t>(max_pos) + 1, -1);
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (target[i] >= 0) {
            out.slot_source[static_cast<std::size_t>(target[i])] =
                static_cast<int>(i);
        }
    }
    return out;
}

}  // namespace akkado
