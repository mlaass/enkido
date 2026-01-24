# **Technical Specification: SquelchEngine**

## **Adaptive Resonant Distortion & Filter System**

Version: 1.0  
Target: C++20  
DSP Focus: Zero-Delay Feedback (ZDF) Filtering, Anti-Aliased Wavefolding, Non-Linear Modulation.

## **1\. System Overview & Architecture**

### **1.1 Core Philosophy**

The SquelchEngine is not a static saturation unit. It is a **reactive system** where signal dynamics (Amplitude/Transient) directly modulate the spectral shaping (Filter) and harmonic generation (Distortion). The "Squelch" character is defined by the interaction between the **Envelope Follower** and the **Resonant Peak**.

### **1.2 Signal Flow Topology**

The audio engine operates on a sample-by-sample basis (required for ZDF stability) with the following routing:

1. **Input Conditioning:** Pre-gain & High-pass (DC blocking).  
2. **Modulation Analysis:** Sidechain/Internal signal analysis (Envelope Follower).  
3. **The "Driver" Stage (Pre-Filter Distortion):** Adds harmonics to clean signals so the filter has content to "grip."  
4. **The Filter Core:** Replaceable filter topologies (Diode, Formant, SVF).  
   * *Key Feature:* Internal Saturation Loop (feedback path non-linearity).  
5. **The "Mangle" Stage (Post-Filter Distortion):** Wavefolding/Hard-Clipping.  
6. **Output:** Mix blend and limiter.

## **2\. Component A: The Modulator Engine (Dynamics)**

The "Squelch" requires a modulator that acts like an analog control voltage (CV), not a digital absolute value.

### **2.1 Analog-Modeled Envelope Follower**

**Requirement:** Must utilize independent Attack and Release coefficients with logarithmic transfer curves to mimic capacitor charge/discharge.

**Algorithm (C++ Pseudo-code):**

// Coefficients calculated from time constants (tau)  
// release\_coeff \= exp(-1.0 / (sampleRate \* releaseTime));  
float processEnvelope(float input, float attack\_coeff, float release\_coeff) {  
    float env\_in \= std::abs(input);  
    float current\_env; // State variable

    if (env\_in \> current\_env)  
        current\_env \= attack\_coeff \* current\_env \+ (1.0 \- attack\_coeff) \* env\_in;  
    else  
        current\_env \= release\_coeff \* current\_env \+ (1.0 \- release\_coeff) \* env\_in;

    return current\_env;  
}

### **2.2 Audio Rate Modulation (FM)**

**Requirement:** Allow the input audio signal to modulate the filter cutoff frequency directly.

* **Math:** $f\_{cutoff}\[n\] \= f\_{base} \\cdot 2^{(ModDepth \\cdot Input\[n\])}$  
* **Acoustic Result:** Creates vocal "growls" and "gurgles" as the filter tracks the waveform's zero crossings.

## **3\. Component B: The Filter Core (The "Voice")**

This is the most critical component. We will implement three selectable topologies.

### **3.1 Topology 1: The "Acid" Ladder (ZDF Diode Simulation)**

Reference: Roland TB-303  
Description: A 4-pole filter where poles are interactively coupled (unbuffered), resulting in a non-linear passband attenuation as resonance increases.  
Implementation Logic (The Mystran Method):  
We must solve the implicit equation for the voltage drop across the diode ladder.

* **State Space:** 4 capacitors ($s\_1, s\_2, s\_3, s\_4$).  
* **Non-Linearity:** $I \= I\_s \\sinh(\\frac{V}{V\_t})$ (using hyperbolic sine for symmetric diode clipping approximations).  
* **Solver:** One-step Newton-Raphson iteration is usually sufficient for audio rates.

Equation:

$$y\[n\] \= x\[n\] \- k \\cdot G(y\[n\])$$

Where $G$ is the low-pass transfer function of the 4 combined poles and $k$ is resonance.

### **3.2 Topology 2: The "Vowel" Bank (Formant Morphing)**

Reference: Sugar Bytes WOW  
Description: Parallel configuration of 3 Band-Pass Filters (BPF).  
**Architecture:**

* **BPF Type:** 2-pole Chamberlin SVF (State Variable Filter) for stability at high Q.  
* **Morphing:** The user selects two vowels (e.g., 'A' and 'I'). The engine interpolates the Center Frequency ($F\_c$) and Gain ($G$) of all 3 filters.

Formant Table (Target Data):  
| Vowel | F1 (Hz) | Gain 1 | F2 (Hz) | Gain 2 | F3 (Hz) | Gain 3 |  
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |  
| A (hod) | 650 | 0dB | 1100 | \-6dB | 2860 | \-20dB |  
| I (bee) | 300 | \-5dB | 2300 | \-10dB | 3000 | \-25dB |  
| U (boot) | 300 | \-5dB | 870 | \-10dB | 2240 | \-25dB |

### **3.3 Topology 3: The "Scream" (MS-20 Sallen-Key)**

Description: A 12dB/oct Low/High pass filter with diode clipping in the feedback loop.  
Key Characteristic: Self-oscillates aggressively. The clipping in the feedback loop prevents volume explosion but squares-off the sine wave resonance, creating a "fuzz-wah" tone.

## **4\. Component C: The Distortion Engine (The "Grit")**

Algorithms must support **ADAA (Antiderivative Anti-Aliasing)** to prevent digital harshness.

### **4.1 Wavefolder (West Coast / Buchla Style)**

Sonic Character: Metallic, hollow, FM-like.  
Algorithm (Sine Map):

$$f(x) \= \\sin(drive \\cdot x)$$

ADAA Implementation:  
To mitigate aliasing, we process the antiderivative ($F\_1$) instead of the direct function.

1. **Antiderivative of sin:** $F\_1(x) \= \-\\cos(x)$  
2. Process:  
   $$y\[n\] \= \\frac{drive}{2} \\cdot \\frac{-\\cos(x\[n\]) \- (-\\cos(x\[n-1\]))}{x\[n\] \- x\[n-1\]}$$

   (Note: Handle the $x\[n\] \\approx x\[n-1\]$ singularity using Taylor expansion).

### **4.2 Asymmetric Diode Clipper (Class A Tube)**

Sonic Character: Warm, thick, generates even harmonics.  
Algorithm:

$$f(x) \= \\begin{cases} 2x & x \< \-0.5 \\\\ \-\\frac{1}{4x} & x \\ge \-0.5 \\text{ (Soft knee logic)} \\end{cases}$$

Alternative Asymmetric Curve:

$$f(x) \= \\frac{x}{(1 \+ |x|^\\nu)^{1/\\nu}} \+ \\text{DC\\\_Offset}$$

### **4.3 Bit-Crush / Sample-Hold**

Sonic Character: Lo-fi, robotic.  
Placement: Post-filter only.  
Algorithm: A "phasor" accumulates rate. When phasor wraps, update the output sample. Between wraps, hold the previous sample.

## **5\. DSP Implementation Strategy**

### **5.1 The "Update" vs "Process" Loop**

To minimize CPU load while maintaining audio-rate modulation:

* **Sample Rate (Audio):**  
  1. Compute Envelope Follower value.  
  2. Update Filter Cutoff ($F\_c$) based on Envelope.  
  3. Recalculate Filter Coefficients (ZDF requires coefficient update per-sample for stability during fast sweeps).  
  4. Run Solver (Newton-Raphson).  
  5. Run Distortion (ADAA).

### **5.2 Optimization Targets**

* **SIMD:** Use AVX2/NEON intrinsics for the parallel Formant Bank (processing 4 filters simultaneously).  
* **Lookup Tables (LUT):** Use LUTs for tanh, sin, and exp functions in the saturation stages if accurate math is too CPU intensive (though modern FPUs often handle these fast enough).

## **6\. Interface Controls (Parameter Mapping)**

The DSP is complex, but the params must be simple.

| Knob Name | DSP Parameter(s) Controlled | Description |
| :---- | :---- | :---- |
| **SQUELCH** | Filter Resonance (Q) \+ Feedback Drive | Controls the "chirp" intensity. |
| **BITE** | Filter Cutoff \+ Env Mod Depth | How much the envelope opens the filter. |
| **MANGLER** | Distortion Drive \+ Wavefold Mix | Blends from Clean \-\> Saturation \-\> Wavefold. |
| **VOWEL** | Formant Interpolation (0.0 \- 1.0) | Morphs between selected vowels. |
| **SPEED** | Envelope Release Time | Fast for "Zap", Slow for "Wah". |
| **DIRT** | Asymmetry Bias | Adds even harmonics (warmth). |

## **7\. Development Roadmap**

1. **Phase 1 (The Test Bench):**  
   * Build a simple C++ harness.  
   * Implement the **Mystran Diode Ladder** class.  
   * Verify resonance behavior (does it self-oscillate?).  
2. **Phase 2 (The Modulation):**  
   * Implement the Envelope Follower.  
   * Connect Envelope \-\> Filter Cutoff.  
   * Tune the Attack/Release curves to match a TB-303 sample.  
3. **Phase 3 (The Distortion):**  
   * Implement ADAA Wavefolder.  
   * Place it *Post-Filter*.  
   * Experiment with placing it *Pre-Filter*.  
4. **Phase 4 (The Polish):**  
   * Add the Formant Bank.  
   * Optimize SIMD.