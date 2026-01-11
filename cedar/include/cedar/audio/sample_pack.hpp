#pragma once

#include "cedar/vm/sample_bank.hpp"
#include "akkado/sample_registry.hpp"
#include <string>
#include <vector>

namespace cedar {

/// Helper for loading collections of samples
class SamplePack {
public:
    struct SampleInfo {
        std::string name;
        std::string filepath;
        std::uint32_t id;
    };

    /// Load a drum kit from a directory
    /// Looks for standard drum sample names (kick.wav, snare.wav, etc.)
    /// @param bank SampleBank to load samples into
    /// @param registry Optional SampleRegistry to register names
    /// @param directory Directory containing WAV files
    /// @return Number of samples loaded
    static int load_drum_kit(SampleBank& bank,
                            akkado::SampleRegistry* registry,
                            const std::string& directory) {
        int loaded = 0;

        // Standard drum kit samples with their IDs
        std::vector<SampleInfo> drum_samples = {
            {"bd", "kick.wav", 1},
            {"bd", "bd.wav", 1},
            {"kick", "kick.wav", 1},
            {"sd", "snare.wav", 2},
            {"sd", "sd.wav", 2},
            {"snare", "snare.wav", 2},
            {"hh", "hihat.wav", 3},
            {"hh", "hh.wav", 3},
            {"hihat", "hihat.wav", 3},
            {"oh", "openhat.wav", 4},
            {"oh", "oh.wav", 4},
            {"cp", "clap.wav", 5},
            {"cp", "cp.wav", 5},
            {"clap", "clap.wav", 5},
            {"rim", "rimshot.wav", 6},
            {"rim", "rim.wav", 6},
            {"tom", "tom.wav", 7},
            {"perc", "perc.wav", 8},
            {"cymbal", "cymbal.wav", 9},
            {"crash", "crash.wav", 10},
            {"cowbell", "cowbell.wav", 11},
            {"shaker", "shaker.wav", 12},
            {"tambourine", "tambourine.wav", 13},
            {"conga", "conga.wav", 14},
            {"bongo", "bongo.wav", 15},
        };

        for (const auto& sample : drum_samples) {
            std::string filepath = directory + "/" + sample.filepath;
            std::uint32_t id = bank.load_wav_file(sample.name, filepath);

            if (id != 0) {
                // Successfully loaded
                if (registry) {
                    registry->register_sample(sample.name, id);
                }
                loaded++;
            }
        }

        return loaded;
    }

    /// Load samples from a list of files
    /// @param bank SampleBank to load samples into
    /// @param registry Optional SampleRegistry to register names
    /// @param samples List of sample info (name, filepath, id)
    /// @return Number of samples loaded
    static int load_samples(SampleBank& bank,
                           akkado::SampleRegistry* registry,
                           const std::vector<SampleInfo>& samples) {
        int loaded = 0;

        for (const auto& sample : samples) {
            std::uint32_t id = bank.load_wav_file(sample.name, sample.filepath);

            if (id != 0) {
                if (registry) {
                    registry->register_sample(sample.name, id);
                }
                loaded++;
            }
        }

        return loaded;
    }

    /// Generate simple synthetic drum samples
    /// Useful for testing or when no WAV files are available
    /// @param bank SampleBank to load samples into
    /// @param registry Optional SampleRegistry to register names
    /// @param sample_rate Sample rate for generated samples
    /// @return Number of samples generated
    static int generate_synthetic_drums(SampleBank& bank,
                                        akkado::SampleRegistry* registry,
                                        float sample_rate = 48000.0f) {
        int loaded = 0;

        // Generate kick drum (sine sweep)
        {
            std::vector<float> kick_data(static_cast<std::size_t>(sample_rate * 0.5f));
            for (std::size_t i = 0; i < kick_data.size(); ++i) {
                float t = static_cast<float>(i) / sample_rate;
                float freq = 150.0f * std::exp(-t * 8.0f);
                float env = std::exp(-t * 6.0f);
                kick_data[i] = std::sin(2.0f * 3.14159f * freq * t) * env;
            }
            std::uint32_t id = bank.load_sample("bd", kick_data.data(),
                                                static_cast<std::uint32_t>(kick_data.size()),
                                                1, sample_rate);
            if (registry && id != 0) {
                registry->register_sample("bd", id);
                registry->register_sample("kick", id);
                loaded++;
            }
        }

        // Generate snare drum (noise + tone)
        {
            std::vector<float> snare_data(static_cast<std::size_t>(sample_rate * 0.2f));
            for (std::size_t i = 0; i < snare_data.size(); ++i) {
                float t = static_cast<float>(i) / sample_rate;
                float env = std::exp(-t * 15.0f);
                float tone = std::sin(2.0f * 3.14159f * 200.0f * t);
                float noise = (static_cast<float>(rand()) / RAND_MAX) * 2.0f - 1.0f;
                snare_data[i] = (tone * 0.3f + noise * 0.7f) * env;
            }
            std::uint32_t id = bank.load_sample("sd", snare_data.data(),
                                                static_cast<std::uint32_t>(snare_data.size()),
                                                1, sample_rate);
            if (registry && id != 0) {
                registry->register_sample("sd", id);
                registry->register_sample("snare", id);
                loaded++;
            }
        }

        // Generate hi-hat (filtered noise)
        {
            std::vector<float> hihat_data(static_cast<std::size_t>(sample_rate * 0.1f));
            for (std::size_t i = 0; i < hihat_data.size(); ++i) {
                float t = static_cast<float>(i) / sample_rate;
                float env = std::exp(-t * 25.0f);
                float noise = (static_cast<float>(rand()) / RAND_MAX) * 2.0f - 1.0f;
                hihat_data[i] = noise * env;
            }
            std::uint32_t id = bank.load_sample("hh", hihat_data.data(),
                                                static_cast<std::uint32_t>(hihat_data.size()),
                                                1, sample_rate);
            if (registry && id != 0) {
                registry->register_sample("hh", id);
                registry->register_sample("hihat", id);
                loaded++;
            }
        }

        // Generate clap (short burst of noise)
        {
            std::vector<float> clap_data(static_cast<std::size_t>(sample_rate * 0.15f));
            for (std::size_t i = 0; i < clap_data.size(); ++i) {
                float t = static_cast<float>(i) / sample_rate;
                float env = std::exp(-t * 20.0f);
                // Add some delay/reverb effect
                float delayed = (i > sample_rate * 0.01f) ? std::exp(-(t - 0.01f) * 20.0f) : 0.0f;
                float noise = (static_cast<float>(rand()) / RAND_MAX) * 2.0f - 1.0f;
                clap_data[i] = noise * (env + delayed * 0.5f);
            }
            std::uint32_t id = bank.load_sample("cp", clap_data.data(),
                                                static_cast<std::uint32_t>(clap_data.size()),
                                                1, sample_rate);
            if (registry && id != 0) {
                registry->register_sample("cp", id);
                registry->register_sample("clap", id);
                loaded++;
            }
        }

        return loaded;
    }
};

}  // namespace cedar
