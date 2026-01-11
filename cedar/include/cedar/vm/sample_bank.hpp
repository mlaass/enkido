#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <fstream>
#include "cedar/audio/wav_loader.hpp"

namespace cedar {

/// Metadata for a loaded audio sample
struct SampleData {
    std::vector<float> data;    // Interleaved audio data
    std::uint32_t channels = 1; // Number of channels (1=mono, 2=stereo)
    float sample_rate = 48000.0f; // Original sample rate
    std::uint32_t frames = 0;   // Number of frames (samples / channels)

    /// Get sample at frame and channel with bounds checking
    [[nodiscard]] float get(std::uint32_t frame, std::uint32_t channel) const {
        if (frame >= frames || channel >= channels) return 0.0f;
        return data[frame * channels + channel];
    }

    /// Get sample with linear interpolation
    [[nodiscard]] float get_interpolated(float position, std::uint32_t channel) const {
        if (position < 0.0f || position >= static_cast<float>(frames) || channel >= channels) {
            return 0.0f;
        }

        std::uint32_t frame0 = static_cast<std::uint32_t>(position);
        std::uint32_t frame1 = frame0 + 1;

        if (frame1 >= frames) {
            return get(frame0, channel);
        }

        float frac = position - static_cast<float>(frame0);
        float sample0 = get(frame0, channel);
        float sample1 = get(frame1, channel);

        return sample0 * (1.0f - frac) + sample1 * frac;
    }

    /// Get length in seconds
    [[nodiscard]] float duration_seconds() const {
        return static_cast<float>(frames) / sample_rate;
    }
};

/// Bank of loaded audio samples
/// Samples are referenced by name or by numeric ID
class SampleBank {
public:
    /// Load a sample from raw float data
    /// @param name Sample name for lookup
    /// @param data Pointer to interleaved audio data
    /// @param num_frames Number of sample frames (samples / channels)
    /// @param channels Number of channels (1=mono, 2=stereo)
    /// @param sample_rate Sample rate in Hz
    /// @return Sample ID, or 0 if failed
    std::uint32_t load_sample(const std::string& name,
                              const float* data,
                              std::uint32_t num_frames,
                              std::uint16_t channels,
                              float sample_rate) {
        // Check for name collision - if name exists, return existing ID
        auto it = name_to_id_.find(name);
        if (it != name_to_id_.end()) {
            return it->second;
        }

        // Overflow check: ensure num_frames * channels doesn't overflow
        if (channels == 0 || num_frames > SIZE_MAX / channels) {
            return 0;  // Invalid parameters or overflow
        }

        std::uint32_t id = next_id_++;

        SampleData sample;
        sample.channels = channels;
        sample.sample_rate = sample_rate;
        sample.frames = num_frames;

        // Copy audio data
        std::size_t total_samples = static_cast<std::size_t>(num_frames) * channels;
        sample.data.resize(total_samples);
        std::memcpy(sample.data.data(), data, total_samples * sizeof(float));

        samples_[id] = std::move(sample);
        name_to_id_[name] = id;

        return id;
    }

    /// Load a sample from a WAV file
    /// @param name Sample name for lookup
    /// @param filepath Path to WAV file
    /// @return Sample ID, or 0 if failed
    std::uint32_t load_wav_file(const std::string& name, const std::string& filepath) {
        WavData wav = WavLoader::load_from_file(filepath);
        if (!wav.success) {
            return 0;
        }

        return load_sample(name, wav.samples.data(), wav.num_frames,
                          wav.channels, static_cast<float>(wav.sample_rate));
    }

    /// Load a sample from WAV data in memory
    /// @param name Sample name for lookup
    /// @param data Pointer to WAV file data
    /// @param size Size of WAV data in bytes
    /// @return Sample ID, or 0 if failed
    std::uint32_t load_wav_memory(const std::string& name,
                                   const std::uint8_t* data,
                                   std::size_t size) {
        WavData wav = WavLoader::load_from_memory(data, size);
        if (!wav.success) {
            return 0;
        }

        return load_sample(name, wav.samples.data(), wav.num_frames,
                          wav.channels, static_cast<float>(wav.sample_rate));
    }

    /// Get sample by ID
    [[nodiscard]] const SampleData* get_sample(std::uint32_t sample_id) const {
        auto it = samples_.find(sample_id);
        if (it != samples_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    /// Get sample by name
    [[nodiscard]] const SampleData* get_sample(const std::string& name) const {
        auto it = name_to_id_.find(name);
        if (it != name_to_id_.end()) {
            return get_sample(it->second);
        }
        return nullptr;
    }

    /// Get sample ID by name
    [[nodiscard]] std::uint32_t get_sample_id(const std::string& name) const {
        auto it = name_to_id_.find(name);
        if (it != name_to_id_.end()) {
            return it->second;
        }
        return 0;
    }

    /// Check if a sample exists
    [[nodiscard]] bool has_sample(const std::string& name) const {
        return name_to_id_.find(name) != name_to_id_.end();
    }

    /// Clear all samples
    void clear() {
        samples_.clear();
        name_to_id_.clear();
        next_id_ = 1;
    }

    /// Get number of loaded samples
    [[nodiscard]] std::size_t size() const {
        return samples_.size();
    }

private:
    std::unordered_map<std::uint32_t, SampleData> samples_;
    std::unordered_map<std::string, std::uint32_t> name_to_id_;
    std::uint32_t next_id_ = 1;  // 0 is reserved for "no sample"
};

}  // namespace cedar
