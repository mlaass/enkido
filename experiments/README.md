# Cedar DSP Experiments

This directory contains Python testing suites for the Cedar audio engine. It uses `pybind11` to compile the C++ DSP core into a Python module (`cedar_core`), allowing for cycle-accurate testing and visualization of audio algorithms using NumPy and Matplotlib.

## Prerequisites

* **CMake** (3.21+)
* **C++ Compiler** (C++20 compliant, e.g., GCC 10+, Clang 12+, MSVC 2019+)
* **Python** (3.10+)
* **Python Dependencies**:
    ```bash
    pip install numpy matplotlib scipy pybind11
    ```

## Building the Bindings

Before running python tests, you must compile the C++ core.

1.  Navigate to the project root.
2.  Configure and build using CMake:

    ```bash
    # Configure (ensure pybind11 is found)
    cmake -B build -DCEDAR_BUILD_TESTS=ON

    # Build the 'cedar_core' target
    cmake --build build --target cedar_core --config Release
    ```

3.  This will generate a shared library (e.g., `cedar_core.cpython-310-darwin.so` or `.pyd` on Windows) inside the `experiments/` folder.

## Running Tests

Once the `cedar_core` module is built, run the experiments:

```bash
# 1. Oscillator Analysis (Waveforms & Aliasing)
python experiments/test_oscillators.py

# 2. Filter Analysis (Bode Plots & Resonance)
python experiments/test_filters.py

# 3. Effects Analysis (Distortion Curves, Reverb Tails, Phaser Spectrograms)
python experiments/test_effects.py