import numpy as np
from scipy.io import wavfile
import os

# --- Configuration ---
TABLE_SIZE = 2048       # Serum/Massive standard
FRAMES_PER_TABLE = 256  # Standard number of frames for smooth morphing
SAMPLE_RATE = 44100     # Metadata only (wavetables are pitchless)
OUTPUT_DIR = "generated_wavetables"

def generate_additive(num_harmonics, amp_func, phase_shift=0):
    """
    Generates a single cycle waveform using additive synthesis.
    This ensures the source material is perfectly band-limited to the table size.
    """
    buffer = np.zeros(TABLE_SIZE)
    t = np.linspace(0, 1, TABLE_SIZE, endpoint=False)

    # We sum harmonics up to the Nyquist limit of the table (size / 2)
    # But usually, for the "base" table, we want as much detail as possible.
    max_harmonics = min(num_harmonics, TABLE_SIZE // 2)

    for k in range(1, max_harmonics + 1):
        amp = amp_func(k)
        if amp != 0:
            buffer += amp * np.sin(2 * np.pi * k * t + phase_shift)

    # Normalize to -1.0 to 1.0
    max_val = np.max(np.abs(buffer))
    if max_val > 0:
        buffer /= max_val

    return buffer

def morph_tables(table_a, table_b, steps):
    """Linear interpolation between two tables over 'steps' frames."""
    frames = []
    for i in range(steps):
        alpha = i / (steps - 1)
        # Linear interpolation: A * (1-alpha) + B * alpha
        frame = table_a * (1.0 - alpha) + table_b * alpha
        frames.append(frame)
    return frames

def save_wavetable(filename, frames):
    """Concatenates frames and saves as 32-bit float WAV."""
    data = np.concatenate(frames)

    # Ensure directory exists
    if not os.path.exists(OUTPUT_DIR):
        os.makedirs(OUTPUT_DIR)

    path = os.path.join(OUTPUT_DIR, filename)
    wavfile.write(path, SAMPLE_RATE, data.astype(np.float32))
    print(f"Generated: {path} ({len(frames)} frames)")

# --- Waveform Definitions ---

def get_sine():
    return generate_additive(1, lambda k: 1.0 if k==1 else 0)

def get_triangle():
    # Odd harmonics, amplitude 1/k^2, alternating signs
    return generate_additive(1024, lambda k: (1/(k**2)) * (1 if k%4==1 else -1) if k%2==1 else 0)

def get_saw():
    # All harmonics, amplitude 1/k
    return generate_additive(1024, lambda k: 1/k)

def get_square():
    # Odd harmonics, amplitude 1/k
    return generate_additive(1024, lambda k: 1/k if k%2==1 else 0)

def get_pulse(width):
    """Naive generation for PWM to handle specific widths easily."""
    t = np.linspace(0, 1, TABLE_SIZE, endpoint=False)
    wave = np.where(t < width, 1.0, -1.0)
    return wave

# --- Generators ---

def generate_basic_shapes():
    """Sine -> Triangle -> Saw -> Square"""
    sine = get_sine()
    tri = get_triangle()
    saw = get_saw()
    square = get_square()

    # Split 256 frames into 3 transition sections (approx 85 frames each)
    frames = []
    frames.extend(morph_tables(sine, tri, 85))
    frames.extend(morph_tables(tri, saw, 85))
    frames.extend(morph_tables(saw, square, 86)) # 85+85+86 = 256

    save_wavetable("Basic_Shapes.wav", frames)

def generate_pwm_sweep():
    """Square wave with duty cycle sweeping from 50% to 5%"""
    frames = []
    for i in range(FRAMES_PER_TABLE):
        # Sweep width from 0.5 (Square) down to 0.05 (Thin Pulse)
        width = 0.5 - (0.45 * (i / FRAMES_PER_TABLE))
        frames.append(get_pulse(width))

    save_wavetable("PWM_Sweep.wav", frames)

def generate_harmonic_sweep():
    """Fundamental -> Add Even Harmonics -> Add Odd Harmonics"""
    frames = []

    # Create spectral frames directly
    t = np.linspace(0, 1, TABLE_SIZE, endpoint=False)

    for i in range(FRAMES_PER_TABLE):
        # Progress 0.0 to 1.0
        prog = i / FRAMES_PER_TABLE

        # Base fundamental
        wave = np.sin(2 * np.pi * t)

        # Gradually introduce higher harmonics based on position
        for h in range(2, 16):
            # Calculate an amplitude envelope for this harmonic
            # Harmonics come in one by one
            threshold = (h / 16.0)
            if prog > threshold:
                amp = min(1.0, (prog - threshold) * 5.0) # Fade in
                amp = amp * (1.0 / h) # Natural spectral roll-off
                wave += amp * np.sin(2 * np.pi * h * t)

        # Normalize
        wave /= np.max(np.abs(wave))
        frames.append(wave)

    save_wavetable("Harmonic_Sweep.wav", frames)

if __name__ == "__main__":
    print("Generating Wavetables...")
    generate_basic_shapes()
    generate_pwm_sweep()
    generate_harmonic_sweep()
    print("Done! Files are in the 'generated_wavetables' folder.")
