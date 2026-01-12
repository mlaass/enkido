"""
DSP Utility Functions
=====================

Helper functions for common DSP operations.
"""

import numpy as np
from typing import Tuple


def db_to_linear(db: float | np.ndarray) -> float | np.ndarray:
    """Convert decibels to linear amplitude.
    
    Args:
        db: Value(s) in decibels
        
    Returns:
        Linear amplitude value(s)
    """
    return 10 ** (db / 20)


def linear_to_db(linear: float | np.ndarray) -> float | np.ndarray:
    """Convert linear amplitude to decibels.
    
    Args:
        linear: Linear amplitude value(s)
        
    Returns:
        Value(s) in decibels
    """
    return 20 * np.log10(np.maximum(linear, 1e-10))


def normalize(signal: np.ndarray, peak: float = 1.0) -> np.ndarray:
    """Normalize signal to specified peak amplitude.
    
    Args:
        signal: Input signal
        peak: Target peak amplitude
        
    Returns:
        Normalized signal
    """
    max_val = np.max(np.abs(signal))
    if max_val > 0:
        return signal * (peak / max_val)
    return signal


def rms(signal: np.ndarray) -> float:
    """Calculate RMS (root mean square) of signal.
    
    Args:
        signal: Input signal
        
    Returns:
        RMS value
    """
    return np.sqrt(np.mean(signal ** 2))


def peak_to_peak(signal: np.ndarray) -> float:
    """Calculate peak-to-peak amplitude.
    
    Args:
        signal: Input signal
        
    Returns:
        Peak-to-peak amplitude
    """
    return np.max(signal) - np.min(signal)


def crest_factor(signal: np.ndarray) -> float:
    """Calculate crest factor (peak / RMS).
    
    Args:
        signal: Input signal
        
    Returns:
        Crest factor
    """
    peak = np.max(np.abs(signal))
    rms_val = rms(signal)
    if rms_val > 0:
        return peak / rms_val
    return 0


def zero_crossings(signal: np.ndarray) -> int:
    """Count zero crossings in signal.
    
    Args:
        signal: Input signal
        
    Returns:
        Number of zero crossings
    """
    return np.sum(np.diff(np.signbit(signal)))


def estimate_frequency(signal: np.ndarray, sample_rate: int = 44100) -> float:
    """Estimate fundamental frequency using zero crossings.
    
    This is a simple estimator - for better accuracy use FFT-based methods.
    
    Args:
        signal: Input signal (should be periodic)
        sample_rate: Sample rate in Hz
        
    Returns:
        Estimated frequency in Hz
    """
    crossings = zero_crossings(signal)
    duration = len(signal) / sample_rate
    # Each full cycle has 2 zero crossings
    return crossings / (2 * duration)


def fade_in(signal: np.ndarray, duration_samples: int) -> np.ndarray:
    """Apply linear fade-in to signal.
    
    Args:
        signal: Input signal
        duration_samples: Fade duration in samples
        
    Returns:
        Signal with fade-in applied
    """
    result = signal.copy()
    fade = np.linspace(0, 1, duration_samples)
    result[:duration_samples] *= fade
    return result


def fade_out(signal: np.ndarray, duration_samples: int) -> np.ndarray:
    """Apply linear fade-out to signal.
    
    Args:
        signal: Input signal
        duration_samples: Fade duration in samples
        
    Returns:
        Signal with fade-out applied
    """
    result = signal.copy()
    fade = np.linspace(1, 0, duration_samples)
    result[-duration_samples:] *= fade
    return result


def crossfade(
    signal1: np.ndarray,
    signal2: np.ndarray,
    duration_samples: int,
) -> np.ndarray:
    """Crossfade between two signals.
    
    Args:
        signal1: First signal
        signal2: Second signal
        duration_samples: Crossfade duration in samples
        
    Returns:
        Crossfaded signal
    """
    fade_out_env = np.linspace(1, 0, duration_samples)
    fade_in_env = np.linspace(0, 1, duration_samples)
    
    # Apply fades
    end1 = signal1.copy()
    end1[-duration_samples:] *= fade_out_env
    
    start2 = signal2.copy()
    start2[:duration_samples] *= fade_in_env
    
    # Overlap and add
    result_len = len(signal1) + len(signal2) - duration_samples
    result = np.zeros(result_len)
    result[:len(signal1)] = end1
    result[len(signal1) - duration_samples:] += start2
    
    return result


def resample(
    signal: np.ndarray,
    original_rate: int,
    target_rate: int,
) -> np.ndarray:
    """Resample signal to different sample rate.
    
    Args:
        signal: Input signal
        original_rate: Original sample rate in Hz
        target_rate: Target sample rate in Hz
        
    Returns:
        Resampled signal
    """
    from scipy.signal import resample as scipy_resample
    
    ratio = target_rate / original_rate
    new_length = int(len(signal) * ratio)
    return scipy_resample(signal, new_length)


def ms_to_samples(ms: float, sample_rate: int = 44100) -> int:
    """Convert milliseconds to samples.
    
    Args:
        ms: Time in milliseconds
        sample_rate: Sample rate in Hz
        
    Returns:
        Number of samples
    """
    return int(ms * sample_rate / 1000)


def samples_to_ms(samples: int, sample_rate: int = 44100) -> float:
    """Convert samples to milliseconds.
    
    Args:
        samples: Number of samples
        sample_rate: Sample rate in Hz
        
    Returns:
        Time in milliseconds
    """
    return samples * 1000 / sample_rate


def freq_to_midi(frequency: float) -> float:
    """Convert frequency to MIDI note number.
    
    Args:
        frequency: Frequency in Hz
        
    Returns:
        MIDI note number (can be fractional)
    """
    return 69 + 12 * np.log2(frequency / 440)


def midi_to_freq(midi_note: float) -> float:
    """Convert MIDI note number to frequency.
    
    Args:
        midi_note: MIDI note number (can be fractional)
        
    Returns:
        Frequency in Hz
    """
    return 440 * 2 ** ((midi_note - 69) / 12)


def note_to_freq(note: str) -> float:
    """Convert note name to frequency.
    
    Args:
        note: Note name like 'A4', 'C#3', 'Bb5'
        
    Returns:
        Frequency in Hz
    """
    note_names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
    
    # Handle flats
    note = note.replace('Db', 'C#').replace('Eb', 'D#').replace('Fb', 'E')
    note = note.replace('Gb', 'F#').replace('Ab', 'G#').replace('Bb', 'A#').replace('Cb', 'B')
    
    # Parse note and octave
    if note[1] == '#':
        name = note[:2]
        octave = int(note[2:])
    else:
        name = note[0]
        octave = int(note[1:])
    
    semitone = note_names.index(name)
    midi = (octave + 1) * 12 + semitone
    
    return midi_to_freq(midi)


def load_audio(path: str) -> Tuple[np.ndarray, int]:
    """Load audio file.
    
    Args:
        path: Path to audio file
        
    Returns:
        Tuple of (signal array, sample rate)
    """
    import soundfile as sf
    return sf.read(path)


def save_audio(
    path: str,
    signal: np.ndarray,
    sample_rate: int = 44100,
):
    """Save signal to audio file.
    
    Args:
        path: Output file path
        signal: Signal array
        sample_rate: Sample rate in Hz
    """
    import soundfile as sf
    
    # Clip to prevent clipping distortion
    signal = np.clip(signal, -1, 1)
    sf.write(path, signal, sample_rate)
