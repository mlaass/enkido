# **Product Requirement Document: "Smooch" Wavetable Oscillator (C++)**

## **1\. Overview**

**Project:** Cedar (Live-Coding Audio Engine)

**Goal:** Implement a production-grade wavetable synthesis engine comparable to Serum or Phase Plant for the Cedar VM.

**Key Constraints:** * **Zero Aliasing:** Must use band-limited mip-mapping.

* **Low Noise Floor:** Must use polynomial (Hermite/Cubic) interpolation.
* **Live-Coding Context:** Separation of heavy data preparation (loading/FFT) and lightweight audio-thread execution.

## **2\. Technical Architecture**

The system is divided into two distinct components to ensure real-time stability:

1. **WavetableBank (Resource / Heap):**
   * Stores the actual sample data.
   * Heavy initialization cost (FFT calculation happens here).
   * Immutable during audio playback.
   * Shared across multiple oscillator instances.
2. **SmoochOpcode (DSP / Stack):**
   * The actual opcode implementation named smooch.
   * Lightweight, per-voice state.
   * Holds phase, frequency, and pointers to the bank.
   * Performs the math (interpolation and lookup) per sample.

## **3\. Data Structures**

### **3.1 The Wavetable Pyramid (Mip-Maps)**

To prevent aliasing, every waveform frame must be pre-calculated into a series of band-limited versions.

* **Base Table Size:** 2048 samples (Standard for Serum/Massive).
* **Mip-Map Levels:** One table per octave.
  * *Table 0:* Full Spectrum (1024 harmonics).
  * *Table 1:* Half Spectrum (512 harmonics).
  * *...*
  * *Table 10:* Fundamental only (1 harmonic).
```cpp
// Represents a single shape (e.g., "Sawtooth")
struct WavetableFrame {
    // [MipLevel][SampleIndex]
    // Inner vector is always size 2048 for ease of memory access (wrapping),
    // even though higher levels contain less frequency content.
    std::vector<std::vector<float>> mipMaps;
};

// Represents the full loadable file (e.g., "Basic Shapes.wt")
struct WavetableBank {
    std::vector<WavetableFrame> frames; // Array of morphable frames
    int tableSize = 2048;
};
```

## **4\. Algorithm 1: Pre-Processing (KissFFT Generation)**

This process runs **offline** (at load time). It converts a raw waveform into the aliasing-safe pyramid.

**Dependencies:** kiss\_fftr (Real-to-Complex FFT).

### **Steps:**

1. **Load/Generate Base Waveform:** Create the source array of 2048 floats.
2. **Forward FFT:** Convert the time-domain signal to the frequency domain (complex numbers).
3. **The Loop (Generate Mip-Maps):**
   * For each octave $k$ (from 0 to max\_octaves):
   * Calculate cutoff\_bin. For Table 0, keep all bins. For Table 1, keep lower 50%. For Table 2, keep lower 25%.
   * **Brickwall Filter:** Set the Magnitude of all complex bins *above* cutoff\_bin to 0.0 + 0.0i.
   * **Inverse FFT (IFFT):** Convert back to time-domain.
   * **Normalize:** Ensure amplitude remains consistent.
   * **Store:** Save result into WavetableFrame.mipMaps[k].

### **C++ Logic (Pseudo-Implementation)**
```cpp
#include "kiss_fftr.h"

void generateMipMaps(WavetableFrame& frame, const std::vector<float>& sourceData) {
    int N = sourceData.size(); // 2048
    kiss_fftr_cfg fwd = kiss_fftr_alloc(N, 0, NULL, NULL);
    kiss_fftr_cfg inv = kiss_fftr_alloc(N, 1, NULL, NULL);

    std::vector<kiss_fft_cpx> spectrum(N / 2 + 1);
    std::vector<float> timeData = sourceData;

    // 1. Initial Forward FFT to get full spectrum
    kiss_fftr(fwd, timeData.data(), spectrum.data());

    // 2. Generate ~10 tables (one per octave)
    int numTables = log2(N);

    for (int i = 0; i < numTables; ++i) {
        // Calculate max harmonic for this level
        // Level 0: N/2, Level 1: N/4, Level 2: N/8...
        int maxBin = (N / 2) >> i;

        // Copy original spectrum
        std::vector<kiss_fft_cpx> filteredSpectrum = spectrum;

        // Zero out bins above maxBin (The "Brickwall")
        for (int bin = maxBin; bin < (N / 2 + 1); ++bin) {
            filteredSpectrum[bin].r = 0.0f;
            filteredSpectrum[bin].i = 0.0f;
        }

        // Inverse FFT
        std::vector<float> outputWave(N);
        kiss_fftri(inv, filteredSpectrum.data(), outputWave.data());

        // Scaling (KissFFT requires division by N after IFFT)
        for (auto& s : outputWave) s /= (float)N;

        frame.mipMaps.push_back(outputWave);
    }
}
```
## **5. Algorithm 2: The Runtime Opcode (Smooch)**

This function runs inside the audio thread. It must be lock-free and efficient.

### **Controls (Inputs)**

1. Freq: Oscillator frequency in Hz.
2. Phase: Phase offset (0.0 to 1.0).
3. TablePos: The morph position (0.0 to Bank.frames.size()).
4. TableID: Which bank to use.

### **State (Persistent Memory)**

1. currentPhase (double): The running phase accumulator.

### **Execution Flow**

#### **Step 1: Phase Accumulation**

Increment phase based on frequency. Use double for precision.

$$ Inc = Freq / SampleRate Phase_{new} = (Phase_{old} + Inc) % 1.0 $$

#### **Step 2: Mip-Map Selection (Anti-Aliasing)**

We determine which "row" of the table pyramid to read to ensure the highest harmonic is below Nyquist.

$$MaxHarmonic = \\frac{SampleRate}{2 \\times Freq}$$$$TableIndex = \\text{Clamp}(\\lfloor \\log\_2(2048 / MaxHarmonic) \\rfloor, 0, MaxLevels)$$

* *Low Pitch* -> Low Index (High detail).
* *High Pitch* -> High Index (Blurry, no aliasing).

#### **Step 3: Frame Selection (Morphing)**

We determine which two shapes we are between (e.g., morphing from Square $2$ to Triangle $3$ ).

$$Frame_A = \lfloor TablePos \rfloor$$$$Frame_B = Frame_A + 1$$$$MorphFactor = TablePos - Frame_A$$

#### **Step 4: 4-Point Hermite Interpolation**

Linear interpolation is too noisy. We fetch 4 samples for *each* frame (A and B) and interpolate.

**The Hermite Function:**
```cpp
inline float hermite(float x, float y0, float y1, float y2, float y3) {
    float c0 = y1;
    float c1 = 0.5f * (y2 - y0);
    float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * x + c2) * x + c1) * x + c0;
}
```

**Reading the Table:**

1. Calculate read pointer: ptr = Phase * 2048\.
2. Get integer part idx and fractional part frac.
3. Read indices: idx-1, idx, idx+1, idx+2 (using bitwise AND & 2047 for wrapping).
4. Compute SignalA = hermite(...) from Frame A's mip-map.
5. Compute SignalB = hermite(...) from Frame B's mip-map.

#### **Step 5: Final Output**

Linearly interpolate the results of the two Hermite calculations based on the morph position.

$$Output = SignalA + (SignalB - SignalA) \times MorphFactor$$

## **6\. Implementation Summary (The Opcode Function)**
```cpp
struct OscState {
    double phaseAccumulator = 0.0;
};

// The 'smooch' opcode implementation
// Returns a single audio sample
float smooch(OscState& state, const WavetableBank& bank,
             float freqHz, float phaseOffset, float tablePos, float sampleRate) {

    // 1\. Calculate Phase
    double inc = freqHz / sampleRate;
    state.phaseAccumulator += inc;
    if (state.phaseAccumulator >= 1.0) state.phaseAccumulator -= 1.0;

    // Apply Phase Offset (Modulation)
    double finalPhase = state.phaseAccumulator + phaseOffset;
    finalPhase -= floor(finalPhase); // Wrap 0.0-1.0

    // 2\. Select Mip-Map Level (Anti-Aliasing)
    // Note: 2048 is table size.
    // This formula finds the table where the highest harmonic fits under Nyquist.
    float nyquist = sampleRate * 0.5f;
    float maxHarmonic = nyquist / freqHz;
    // Calculate inverse log2 effectively
    int mipLevel = 0;
    // Pre-calculated optimization: float numHarmonics = 1024.0f;
    // while (numHarmonics > maxHarmonic && mipLevel < MAX\_MIPS) {
    //    numHarmonics *= 0.5f; mipLevel++;
    // }
    // OR standard log math:
    mipLevel = std::max(0, (int)(log2(2048.0f / 2.0f) - log2(maxHarmonic)));

    // Safety clamp
    if (mipLevel >= bank.frames[0].mipMaps.size())
        mipLevel = bank.frames[0].mipMaps.size() - 1;

    // 3\. Determine Frames for Morphing
    int frameIdxA = (int)tablePos;
    int frameIdxB = frameIdxA + 1;
    float morphFrac = tablePos - frameIdxA;

    // Wrap frames if we are at the end of the bank
    if (frameIdxA >= bank.frames.size()) frameIdxA = bank.frames.size() - 1;
    if (frameIdxB >= bank.frames.size()) frameIdxB = 0; // Or clamp depending on style

    // 4\. Get Data Pointers
    const auto& tableA = bank.frames[frameIdxA].mipMaps[mipLevel];
    const auto& tableB = bank.frames[frameIdxB].mipMaps[mipLevel];

    // 5\. Calculate Interpolation Indices
    float readPos = finalPhase * 2048.0f;
    int intPos = (int)readPos;
    float fracPos = readPos - intPos;
    int mask = 2047;

    // 6\. Perform Hermite Interpolation on Frame A
    float a0 = tableA[(intPos - 1) & mask];
    float a1 = tableA[intPos];
    float a2 = tableA[(intPos + 1) & mask];
    float a3 = tableA[(intPos + 2) & mask];
    float sigA = hermite(fracPos, a0, a1, a2, a3);

    // 7\. Perform Hermite Interpolation on Frame B
    float b0 = tableB[(intPos - 1) & mask];
    float b1 = tableB[intPos];
    float b2 = tableB[(intPos + 1) & mask];
    float b3 = tableB[(intPos + 2) & mask];
    float sigB = hermite(fracPos, b0, b1, b2, b3);

    // 8\. Morph and Return
    return sigA + (sigB - sigA) * morphFrac;
}
```
