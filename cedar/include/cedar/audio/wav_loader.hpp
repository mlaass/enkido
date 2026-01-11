#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <cstring>

namespace cedar {

struct WavData {
    std::vector<float> samples;  // Interleaved samples (L, R, L, R for stereo)
    std::uint32_t sample_rate;
    std::uint16_t channels;
    std::uint32_t num_frames;    // Number of sample frames (samples / channels)
    bool success;
    std::string error_message;
};

class WavLoader {
public:
    static WavData load_from_file(const std::string& filepath) {
        WavData result{};
        result.success = false;

        // Open file
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            result.error_message = "Failed to open file: " + filepath;
            return result;
        }

        // Get file size
        std::streamsize file_size = file.tellg();
        if (file_size <= 0) {
            result.error_message = "Empty or invalid file: " + filepath;
            return result;
        }

        // Read entire file into memory
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(file_size));
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(buffer.data()), file_size);

        if (!file) {
            result.error_message = "Failed to read file: " + filepath;
            return result;
        }

        // Delegate to memory loader
        return load_from_memory(buffer.data(), buffer.size());
    }

    static WavData load_from_memory(const std::uint8_t* data, std::size_t size) {
        WavData result{};
        result.success = false;

        if (size < 44) {
            result.error_message = "Data too small to be a valid WAV file";
            return result;
        }

        std::size_t offset = 0;

        // Read RIFF header
        if (std::strncmp(reinterpret_cast<const char*>(data), "RIFF", 4) != 0) {
            result.error_message = "Not a valid WAV file (missing RIFF header)";
            return result;
        }
        offset += 8;  // Skip RIFF + size

        // Read WAVE header
        if (std::strncmp(reinterpret_cast<const char*>(data + offset), "WAVE", 4) != 0) {
            result.error_message = "Not a valid WAV file (missing WAVE header)";
            return result;
        }
        offset += 4;

        // Find fmt chunk
        while (offset + 8 <= size) {
            const char* chunk_id = reinterpret_cast<const char*>(data + offset);
            std::uint32_t chunk_size;
            std::memcpy(&chunk_size, data + offset + 4, 4);
            offset += 8;

            if (std::strncmp(chunk_id, "fmt ", 4) == 0) {
                if (offset + chunk_size > size) {
                    result.error_message = "Corrupt WAV file (fmt chunk exceeds file size)";
                    return result;
                }

                std::uint16_t audio_format;
                std::memcpy(&audio_format, data + offset, 2);

                if (audio_format != 1 && audio_format != 3) {
                    result.error_message = "Unsupported audio format";
                    return result;
                }

                std::memcpy(&result.channels, data + offset + 2, 2);
                std::memcpy(&result.sample_rate, data + offset + 4, 4);

                std::uint16_t bits_per_sample;
                std::memcpy(&bits_per_sample, data + offset + 14, 2);

                offset += chunk_size;

                // Find data chunk
                while (offset + 8 <= size) {
                    chunk_id = reinterpret_cast<const char*>(data + offset);
                    std::memcpy(&chunk_size, data + offset + 4, 4);
                    offset += 8;

                    if (std::strncmp(chunk_id, "data", 4) == 0) {
                        if (offset + chunk_size > size) {
                            result.error_message = "Corrupt WAV file (data chunk exceeds file size)";
                            return result;
                        }

                        std::uint32_t num_samples = chunk_size / (bits_per_sample / 8);
                        result.num_frames = num_samples / result.channels;
                        result.samples.resize(num_samples);

                        if (audio_format == 3) {
                            // IEEE float
                            std::memcpy(result.samples.data(), data + offset, chunk_size);
                        } else if (bits_per_sample == 16) {
                            // 16-bit PCM
                            const std::int16_t* pcm_data = 
                                reinterpret_cast<const std::int16_t*>(data + offset);
                            for (std::size_t i = 0; i < num_samples; ++i) {
                                result.samples[i] = pcm_data[i] / 32768.0f;
                            }
                        } else if (bits_per_sample == 24) {
                            // 24-bit PCM
                            for (std::uint32_t i = 0; i < num_samples; ++i) {
                                const std::uint8_t* bytes = data + offset + (i * 3);
                                std::int32_t value = (bytes[2] << 24) | 
                                                    (bytes[1] << 16) | 
                                                    (bytes[0] << 8);
                                value >>= 8;
                                result.samples[i] = value / 8388608.0f;
                            }
                        } else if (bits_per_sample == 32) {
                            // 32-bit PCM
                            const std::int32_t* pcm_data = 
                                reinterpret_cast<const std::int32_t*>(data + offset);
                            for (std::size_t i = 0; i < num_samples; ++i) {
                                result.samples[i] = pcm_data[i] / 2147483648.0f;
                            }
                        } else {
                            result.error_message = "Unsupported bit depth";
                            return result;
                        }

                        result.success = true;
                        return result;
                    } else {
                        offset += chunk_size;
                    }
                }

                result.error_message = "No data chunk found";
                return result;
            } else {
                offset += chunk_size;
            }
        }

        result.error_message = "No fmt chunk found";
        return result;
    }
};

}  // namespace cedar
