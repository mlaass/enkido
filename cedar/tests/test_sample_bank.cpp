#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cedar/vm/sample_bank.hpp>

#include <cmath>
#include <vector>
#include <cstdint>
#include <fstream>
#include <filesystem>

using namespace cedar;
using Catch::Matchers::WithinAbs;

// Helper to create test WAV data in memory
std::vector<std::uint8_t> create_test_wav(int num_samples, int channels, int sample_rate) {
    std::vector<std::uint8_t> data;

    // RIFF header
    data.push_back('R'); data.push_back('I'); data.push_back('F'); data.push_back('F');

    // File size (will fill in later)
    std::uint32_t file_size = 36 + num_samples * channels * 2;
    for (int i = 0; i < 4; ++i) {
        data.push_back((file_size >> (i * 8)) & 0xFF);
    }

    data.push_back('W'); data.push_back('A'); data.push_back('V'); data.push_back('E');

    // fmt chunk
    data.push_back('f'); data.push_back('m'); data.push_back('t'); data.push_back(' ');

    // fmt chunk size (16 for PCM)
    std::uint32_t fmt_size = 16;
    for (int i = 0; i < 4; ++i) {
        data.push_back((fmt_size >> (i * 8)) & 0xFF);
    }

    // Audio format (1 = PCM)
    data.push_back(1); data.push_back(0);

    // Channels
    data.push_back(channels & 0xFF); data.push_back((channels >> 8) & 0xFF);

    // Sample rate
    for (int i = 0; i < 4; ++i) {
        data.push_back((sample_rate >> (i * 8)) & 0xFF);
    }

    // Byte rate
    std::uint32_t byte_rate = sample_rate * channels * 2;
    for (int i = 0; i < 4; ++i) {
        data.push_back((byte_rate >> (i * 8)) & 0xFF);
    }

    // Block align
    std::uint16_t block_align = channels * 2;
    data.push_back(block_align & 0xFF); data.push_back((block_align >> 8) & 0xFF);

    // Bits per sample
    data.push_back(16); data.push_back(0);

    // data chunk
    data.push_back('d'); data.push_back('a'); data.push_back('t'); data.push_back('a');

    // data chunk size
    std::uint32_t data_size = num_samples * channels * 2;
    for (int i = 0; i < 4; ++i) {
        data.push_back((data_size >> (i * 8)) & 0xFF);
    }

    // Sample data - sine wave
    for (int s = 0; s < num_samples; ++s) {
        float phase = static_cast<float>(s) / static_cast<float>(num_samples);
        float value = std::sin(phase * 6.28318f);
        std::int16_t sample = static_cast<std::int16_t>(value * 32767.0f);

        for (int c = 0; c < channels; ++c) {
            data.push_back(sample & 0xFF);
            data.push_back((sample >> 8) & 0xFF);
        }
    }

    return data;
}

// ============================================================================
// Unit Tests [sample_bank]
// ============================================================================

TEST_CASE("SampleBank basic operations", "[sample_bank]") {
    SampleBank bank;

    SECTION("load_sample returns valid ID") {
        std::vector<float> data(100, 0.5f);

        std::uint32_t id = bank.load_sample("test", data.data(), 100, 1, 44100.0f);

        CHECK(id != 0);
    }

    SECTION("load_sample stores correct data") {
        std::vector<float> data(100);
        for (std::size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<float>(i) * 0.01f;
        }

        std::uint32_t id = bank.load_sample("stored", data.data(), 100, 1, 44100.0f);
        const SampleData* sample = bank.get_sample(id);

        REQUIRE(sample != nullptr);
        CHECK(sample->frames == 100);
        CHECK(sample->channels == 1);
        CHECK_THAT(sample->sample_rate, WithinAbs(44100.0f, 1e-6f));

        for (std::size_t i = 0; i < 100; ++i) {
            CHECK_THAT(sample->get(i, 0), WithinAbs(static_cast<float>(i) * 0.01f, 1e-6f));
        }
    }

    SECTION("get_sample by ID retrieval") {
        std::vector<float> data(50, 1.0f);
        std::uint32_t id = bank.load_sample("by_id", data.data(), 50, 1, 48000.0f);

        const SampleData* sample = bank.get_sample(id);
        REQUIRE(sample != nullptr);
        CHECK(sample->frames == 50);
    }

    SECTION("get_sample by name retrieval") {
        std::vector<float> data(50, 1.0f);
        bank.load_sample("by_name_test", data.data(), 50, 1, 48000.0f);

        const SampleData* sample = bank.get_sample("by_name_test");
        REQUIRE(sample != nullptr);
        CHECK(sample->frames == 50);
    }

    SECTION("get_sample_id returns correct ID") {
        std::vector<float> data(100, 0.0f);
        std::uint32_t expected_id = bank.load_sample("id_lookup", data.data(), 100, 1, 44100.0f);

        std::uint32_t id = bank.get_sample_id("id_lookup");
        CHECK(id == expected_id);
    }

    SECTION("has_sample returns correct values") {
        CHECK_FALSE(bank.has_sample("nonexistent"));

        std::vector<float> data(10, 0.0f);
        bank.load_sample("exists", data.data(), 10, 1, 44100.0f);

        CHECK(bank.has_sample("exists"));
        CHECK_FALSE(bank.has_sample("still_nonexistent"));
    }

    SECTION("clear removes all samples") {
        std::vector<float> data(10, 0.0f);
        bank.load_sample("s1", data.data(), 10, 1, 44100.0f);
        bank.load_sample("s2", data.data(), 10, 1, 44100.0f);
        bank.load_sample("s3", data.data(), 10, 1, 44100.0f);

        REQUIRE(bank.size() == 3);

        bank.clear();

        CHECK(bank.size() == 0);
        CHECK_FALSE(bank.has_sample("s1"));
        CHECK_FALSE(bank.has_sample("s2"));
        CHECK_FALSE(bank.has_sample("s3"));
    }

    SECTION("size tracks sample count") {
        CHECK(bank.size() == 0);

        std::vector<float> data(10, 0.0f);
        bank.load_sample("s1", data.data(), 10, 1, 44100.0f);
        CHECK(bank.size() == 1);

        bank.load_sample("s2", data.data(), 10, 1, 44100.0f);
        CHECK(bank.size() == 2);

        bank.clear();
        CHECK(bank.size() == 0);
    }
}

TEST_CASE("SampleData interpolation", "[sample_bank]") {
    SampleBank bank;

    SECTION("get_interpolated linear interpolation") {
        std::vector<float> data = {0.0f, 1.0f, 0.0f};  // Triangle wave
        std::uint32_t id = bank.load_sample("interp", data.data(), 3, 1, 44100.0f);
        const SampleData* sample = bank.get_sample(id);
        REQUIRE(sample != nullptr);

        // At exact positions
        CHECK_THAT(sample->get_interpolated(0.0f, 0), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(sample->get_interpolated(1.0f, 0), WithinAbs(1.0f, 1e-6f));
        CHECK_THAT(sample->get_interpolated(2.0f, 0), WithinAbs(0.0f, 1e-6f));

        // Interpolated positions
        CHECK_THAT(sample->get_interpolated(0.5f, 0), WithinAbs(0.5f, 1e-6f));
        CHECK_THAT(sample->get_interpolated(1.5f, 0), WithinAbs(0.5f, 1e-6f));
    }

    SECTION("get_interpolated_looped wraps correctly") {
        std::vector<float> data = {0.0f, 1.0f, 2.0f, 3.0f};
        std::uint32_t id = bank.load_sample("loop", data.data(), 4, 1, 44100.0f);
        const SampleData* sample = bank.get_sample(id);
        REQUIRE(sample != nullptr);

        // Normal positions
        CHECK_THAT(sample->get_interpolated_looped(0.0f, 0), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(sample->get_interpolated_looped(1.0f, 0), WithinAbs(1.0f, 1e-6f));

        // Looped positions
        CHECK_THAT(sample->get_interpolated_looped(4.0f, 0), WithinAbs(0.0f, 1e-6f));
        CHECK_THAT(sample->get_interpolated_looped(5.0f, 0), WithinAbs(1.0f, 1e-6f));
    }

    SECTION("stereo sample access") {
        std::vector<float> data = {
            1.0f, -1.0f,  // Frame 0: L=1, R=-1
            0.5f, -0.5f,  // Frame 1: L=0.5, R=-0.5
        };
        std::uint32_t id = bank.load_sample("stereo", data.data(), 2, 2, 44100.0f);
        const SampleData* sample = bank.get_sample(id);
        REQUIRE(sample != nullptr);

        CHECK_THAT(sample->get(0, 0), WithinAbs(1.0f, 1e-6f));   // Left ch0
        CHECK_THAT(sample->get(0, 1), WithinAbs(-1.0f, 1e-6f));  // Right ch0
        CHECK_THAT(sample->get(1, 0), WithinAbs(0.5f, 1e-6f));   // Left ch1
        CHECK_THAT(sample->get(1, 1), WithinAbs(-0.5f, 1e-6f));  // Right ch1
    }
}

TEST_CASE("SampleBank WAV loading", "[sample_bank]") {
    SampleBank bank;

    SECTION("load_wav_memory loads mono WAV") {
        auto wav_data = create_test_wav(100, 1, 44100);

        std::uint32_t id = bank.load_wav_memory("mono_wav",
            wav_data.data(), wav_data.size());

        CHECK(id != 0);

        const SampleData* sample = bank.get_sample(id);
        REQUIRE(sample != nullptr);
        CHECK(sample->frames == 100);
        CHECK(sample->channels == 1);
        CHECK_THAT(sample->sample_rate, WithinAbs(44100.0f, 1.0f));
    }

    SECTION("load_wav_memory loads stereo WAV") {
        auto wav_data = create_test_wav(50, 2, 48000);

        std::uint32_t id = bank.load_wav_memory("stereo_wav",
            wav_data.data(), wav_data.size());

        CHECK(id != 0);

        const SampleData* sample = bank.get_sample(id);
        REQUIRE(sample != nullptr);
        CHECK(sample->frames == 50);
        CHECK(sample->channels == 2);
        CHECK_THAT(sample->sample_rate, WithinAbs(48000.0f, 1.0f));
    }
}

// ============================================================================
// Edge Cases [sample_bank][edge]
// ============================================================================

TEST_CASE("SampleBank edge cases", "[sample_bank][edge]") {
    SampleBank bank;

    SECTION("get non-existent sample by ID") {
        const SampleData* sample = bank.get_sample(9999u);
        CHECK(sample == nullptr);
    }

    SECTION("get non-existent sample by name") {
        const SampleData* sample = bank.get_sample("does_not_exist");
        CHECK(sample == nullptr);
    }

    SECTION("get_sample_id for non-existent name") {
        std::uint32_t id = bank.get_sample_id("missing");
        CHECK(id == 0);
    }

    SECTION("duplicate name returns existing ID") {
        std::vector<float> data1(10, 1.0f);
        std::vector<float> data2(10, 2.0f);

        std::uint32_t id1 = bank.load_sample("dup", data1.data(), 10, 1, 44100.0f);
        std::uint32_t id2 = bank.load_sample("dup", data2.data(), 10, 1, 44100.0f);

        // Second load returns existing ID (no overwrite)
        CHECK(id1 == id2);

        const SampleData* sample = bank.get_sample("dup");
        REQUIRE(sample != nullptr);
        // Value should be from first load (no overwrite)
        CHECK_THAT(sample->get(0, 0), WithinAbs(1.0f, 1e-6f));
    }

    SECTION("empty sample (num_frames=0)") {
        std::vector<float> data;  // Empty
        std::uint32_t id = bank.load_sample("empty", data.data(), 0, 1, 44100.0f);

        // May return invalid ID or valid ID with 0 frames
        if (id != 0) {
            const SampleData* sample = bank.get_sample(id);
            if (sample != nullptr) {
                CHECK(sample->frames == 0);
            }
        }
    }

    SECTION("large sample") {
        std::vector<float> data(1000000, 0.5f);  // 1M samples
        std::uint32_t id = bank.load_sample("large", data.data(), 1000000, 1, 44100.0f);

        CHECK(id != 0);

        const SampleData* sample = bank.get_sample(id);
        REQUIRE(sample != nullptr);
        CHECK(sample->frames == 1000000);

        // Check some samples
        CHECK_THAT(sample->get(0, 0), WithinAbs(0.5f, 1e-6f));
        CHECK_THAT(sample->get(500000, 0), WithinAbs(0.5f, 1e-6f));
        CHECK_THAT(sample->get(999999, 0), WithinAbs(0.5f, 1e-6f));
    }

    SECTION("SampleData duration_seconds") {
        std::vector<float> data(44100, 0.0f);  // 1 second at 44.1kHz
        std::uint32_t id = bank.load_sample("one_sec", data.data(), 44100, 1, 44100.0f);

        const SampleData* sample = bank.get_sample(id);
        REQUIRE(sample != nullptr);
        CHECK_THAT(sample->duration_seconds(), WithinAbs(1.0f, 0.001f));
    }

    SECTION("SampleData get with out of bounds") {
        std::vector<float> data = {1.0f, 2.0f, 3.0f};
        std::uint32_t id = bank.load_sample("bounds", data.data(), 3, 1, 44100.0f);

        const SampleData* sample = bank.get_sample(id);
        REQUIRE(sample != nullptr);

        // Out of bounds should return 0 or last sample (implementation defined)
        float oob = sample->get(100, 0);
        // Just verify no crash
        (void)oob;
    }

    SECTION("invalid WAV data") {
        std::vector<std::uint8_t> garbage = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

        std::uint32_t id = bank.load_wav_memory("garbage",
            garbage.data(), garbage.size());

        CHECK(id == 0);
    }

    SECTION("truncated WAV data") {
        auto wav_data = create_test_wav(100, 1, 44100);
        // Truncate to header only
        wav_data.resize(44);

        std::uint32_t id = bank.load_wav_memory("truncated",
            wav_data.data(), wav_data.size());

        CHECK(id == 0);
    }

    SECTION("many samples") {
        for (int i = 0; i < 100; ++i) {
            std::vector<float> data(10, static_cast<float>(i));
            std::string name = "sample_" + std::to_string(i);
            bank.load_sample(name.c_str(), data.data(), 10, 1, 44100.0f);
        }

        CHECK(bank.size() == 100);

        // Verify random access
        const SampleData* s50 = bank.get_sample("sample_50");
        REQUIRE(s50 != nullptr);
        CHECK_THAT(s50->get(0, 0), WithinAbs(50.0f, 1e-6f));
    }
}

// ============================================================================
// Stress Tests [sample_bank][stress]
// ============================================================================

TEST_CASE("SampleBank stress test", "[sample_bank][stress]") {
    SampleBank bank;

    SECTION("load and access many samples") {
        for (int i = 0; i < 500; ++i) {
            std::vector<float> data(100 + i, static_cast<float>(i) * 0.001f);
            std::string name = "stress_" + std::to_string(i);
            std::uint32_t id = bank.load_sample(name.c_str(), data.data(),
                static_cast<std::uint32_t>(100 + i), 1, 44100.0f);
            REQUIRE(id != 0);
        }

        CHECK(bank.size() == 500);

        // Access all samples
        for (int i = 0; i < 500; ++i) {
            std::string name = "stress_" + std::to_string(i);
            const SampleData* sample = bank.get_sample(name.c_str());
            REQUIRE(sample != nullptr);
            CHECK(sample->frames == static_cast<std::uint32_t>(100 + i));
        }
    }

    SECTION("interpolated playback simulation") {
        std::vector<float> data(4410, 0.0f);  // 100ms at 44.1kHz
        for (std::size_t i = 0; i < data.size(); ++i) {
            data[i] = std::sin(static_cast<float>(i) * 0.1f);
        }

        std::uint32_t id = bank.load_sample("playback_test", data.data(), 4410, 1, 44100.0f);
        const SampleData* sample = bank.get_sample(id);
        REQUIRE(sample != nullptr);

        // Simulate playback with resampling
        float position = 0.0f;
        float rate = 1.5f;  // 1.5x speed
        float output_sum = 0.0f;

        for (int i = 0; i < 10000; ++i) {
            float val = sample->get_interpolated_looped(position, 0);
            output_sum += val;
            position += rate;
            if (position >= static_cast<float>(sample->frames)) {
                position -= static_cast<float>(sample->frames);
            }
        }

        // Just verify it ran without issues
        CHECK(std::isfinite(output_sum));
    }
}
