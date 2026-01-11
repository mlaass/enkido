#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace akkado {

/// Registry for mapping sample names to numeric IDs during compilation
/// This allows mini-notation sample tokens (e.g., "bd", "sd:2") to be
/// resolved to sample IDs that the Cedar VM can use.
class SampleRegistry {
public:
    /// Register a sample name with a specific ID
    /// @param name Sample name (e.g., "bd", "snare", "kick")
    /// @param id Numeric sample ID (must match ID in Cedar's SampleBank)
    /// @return true if registered successfully, false if name already exists
    bool register_sample(const std::string& name, std::uint32_t id) {
        if (name_to_id_.find(name) != name_to_id_.end()) {
            return false;  // Already registered
        }
        name_to_id_[name] = id;
        return true;
    }

    /// Get sample ID by name
    /// @param name Sample name
    /// @return Sample ID, or 0 if not found (0 = no sample)
    [[nodiscard]] std::uint32_t get_id(const std::string& name) const {
        auto it = name_to_id_.find(name);
        if (it != name_to_id_.end()) {
            return it->second;
        }
        return 0;  // Not found
    }

    /// Check if a sample name is registered
    [[nodiscard]] bool has_sample(const std::string& name) const {
        return name_to_id_.find(name) != name_to_id_.end();
    }

    /// Get all registered sample names
    [[nodiscard]] std::vector<std::string> get_all_names() const {
        std::vector<std::string> names;
        names.reserve(name_to_id_.size());
        for (const auto& [name, id] : name_to_id_) {
            names.push_back(name);
        }
        return names;
    }

    /// Clear all registered samples
    void clear() {
        name_to_id_.clear();
    }

    /// Get number of registered samples
    [[nodiscard]] std::size_t size() const {
        return name_to_id_.size();
    }

    /// Register default drum samples with standard names
    /// This provides a convenient set of common sample names
    /// IDs start from 1 (0 is reserved for "no sample")
    void register_defaults() {
        // Drum kit samples (IDs 1-20)
        register_sample("bd", 1);      // Bass drum / kick
        register_sample("kick", 1);    // Alias for bd
        register_sample("sd", 2);      // Snare drum
        register_sample("snare", 2);   // Alias for sd
        register_sample("hh", 3);      // Hi-hat (closed)
        register_sample("hihat", 3);   // Alias for hh
        register_sample("oh", 4);      // Open hi-hat
        register_sample("cp", 5);      // Clap
        register_sample("clap", 5);    // Alias for cp
        register_sample("rim", 6);     // Rimshot
        register_sample("tom", 7);     // Tom
        register_sample("perc", 8);    // Percussion
        register_sample("cymbal", 9);  // Cymbal
        register_sample("crash", 10);  // Crash cymbal
        
        // Additional percussion (IDs 11-20)
        register_sample("cowbell", 11);
        register_sample("shaker", 12);
        register_sample("tambourine", 13);
        register_sample("conga", 14);
        register_sample("bongo", 15);
    }

private:
    std::unordered_map<std::string, std::uint32_t> name_to_id_;
};

}  // namespace akkado
