"""
DSP Visualization Functions
============================

Plotting functions for DSP analysis results.
All functions return the matplotlib figure for further customization.
"""

import numpy as np
import matplotlib.pyplot as plt
from matplotlib.figure import Figure
from typing import Tuple


# Set up a nice style for technical plots
plt.style.use('seaborn-v0_8-whitegrid')


def plot_spectrum(
    freqs: np.ndarray,
    magnitude_db: np.ndarray,
    title: str = "Frequency Spectrum",
    xlim: Tuple[float, float] | None = None,
    ylim: Tuple[float, float] | None = None,
    log_freq: bool = True,
    figsize: Tuple[float, float] = (12, 6),
) -> Figure:
    """Plot frequency spectrum.
    
    Args:
        freqs: Frequency values in Hz
        magnitude_db: Magnitude values in dB
        title: Plot title
        xlim: Optional x-axis limits (min_freq, max_freq)
        ylim: Optional y-axis limits (min_db, max_db)
        log_freq: If True, use logarithmic frequency axis
        figsize: Figure size
        
    Returns:
        matplotlib Figure
    """
    fig, ax = plt.subplots(figsize=figsize)
    
    ax.plot(freqs, magnitude_db, linewidth=0.8)
    
    if log_freq:
        ax.set_xscale('log')
    
    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Magnitude (dB)')
    ax.set_title(title)
    
    if xlim:
        ax.set_xlim(xlim)
    else:
        ax.set_xlim(20, freqs[-1])
    
    if ylim:
        ax.set_ylim(ylim)
    
    ax.grid(True, which='both', linestyle='-', alpha=0.3)
    
    plt.tight_layout()
    return fig


def plot_frequency_response(
    freqs: np.ndarray,
    magnitude_db: np.ndarray,
    phase_deg: np.ndarray | None = None,
    title: str = "Frequency Response",
    xlim: Tuple[float, float] | None = None,
    figsize: Tuple[float, float] = (12, 8),
) -> Figure:
    """Plot filter frequency response (magnitude and optional phase).
    
    Args:
        freqs: Frequency values in Hz
        magnitude_db: Magnitude values in dB
        phase_deg: Optional phase values in degrees
        title: Plot title
        xlim: Optional x-axis limits
        figsize: Figure size
        
    Returns:
        matplotlib Figure
    """
    if phase_deg is not None:
        fig, (ax1, ax2) = plt.subplots(2, 1, figsize=figsize, sharex=True)
    else:
        fig, ax1 = plt.subplots(figsize=(figsize[0], figsize[1] // 2))
        ax2 = None
    
    # Magnitude plot
    ax1.semilogx(freqs, magnitude_db, linewidth=1.2)
    ax1.set_ylabel('Magnitude (dB)')
    ax1.set_title(title)
    ax1.grid(True, which='both', linestyle='-', alpha=0.3)
    
    # Common cutoff reference lines
    ax1.axhline(-3, color='red', linestyle='--', alpha=0.5, label='-3 dB')
    ax1.legend(loc='upper right')
    
    if xlim:
        ax1.set_xlim(xlim)
    else:
        ax1.set_xlim(20, max(freqs))
    
    # Phase plot
    if ax2 is not None:
        ax2.semilogx(freqs, phase_deg, linewidth=1.2, color='orange')
        ax2.set_xlabel('Frequency (Hz)')
        ax2.set_ylabel('Phase (degrees)')
        ax2.grid(True, which='both', linestyle='-', alpha=0.3)
        ax2.set_ylim(-180, 180)
    else:
        ax1.set_xlabel('Frequency (Hz)')
    
    plt.tight_layout()
    return fig


def plot_spectrogram(
    times: np.ndarray,
    freqs: np.ndarray,
    power_db: np.ndarray,
    title: str = "Spectrogram",
    ylim: Tuple[float, float] | None = None,
    vmin: float | None = None,
    vmax: float | None = None,
    figsize: Tuple[float, float] = (14, 6),
) -> Figure:
    """Plot spectrogram (time-frequency representation).
    
    Args:
        times: Time values in seconds
        freqs: Frequency values in Hz
        power_db: Power values in dB (2D array)
        title: Plot title
        ylim: Optional frequency axis limits
        vmin: Minimum dB value for colormap
        vmax: Maximum dB value for colormap
        figsize: Figure size
        
    Returns:
        matplotlib Figure
    """
    fig, ax = plt.subplots(figsize=figsize)
    
    if vmax is None:
        vmax = np.max(power_db)
    if vmin is None:
        vmin = vmax - 80
    
    mesh = ax.pcolormesh(
        times, freqs, power_db,
        shading='gouraud',
        vmin=vmin, vmax=vmax,
        cmap='magma'
    )
    
    ax.set_ylabel('Frequency (Hz)')
    ax.set_xlabel('Time (s)')
    ax.set_title(title)
    
    if ylim:
        ax.set_ylim(ylim)
    
    cbar = plt.colorbar(mesh, ax=ax)
    cbar.set_label('Power (dB)')
    
    plt.tight_layout()
    return fig


def plot_impulse_response(
    ir: np.ndarray,
    sample_rate: int = 44100,
    title: str = "Impulse Response",
    xlim: Tuple[float, float] | None = None,
    figsize: Tuple[float, float] = (12, 6),
) -> Figure:
    """Plot impulse response in time domain.
    
    Args:
        ir: Impulse response samples
        sample_rate: Sample rate in Hz
        title: Plot title
        xlim: Optional time axis limits in seconds
        figsize: Figure size
        
    Returns:
        matplotlib Figure
    """
    fig, ax = plt.subplots(figsize=figsize)
    
    time = np.arange(len(ir)) / sample_rate * 1000  # Convert to ms
    
    ax.plot(time, ir, linewidth=0.8)
    ax.set_xlabel('Time (ms)')
    ax.set_ylabel('Amplitude')
    ax.set_title(title)
    ax.grid(True, alpha=0.3)
    
    if xlim:
        ax.set_xlim(xlim[0] * 1000, xlim[1] * 1000)
    
    ax.axhline(0, color='gray', linewidth=0.5)
    
    plt.tight_layout()
    return fig


def plot_step_response(
    response: np.ndarray,
    sample_rate: int = 44100,
    title: str = "Step Response",
    xlim: Tuple[float, float] | None = None,
    figsize: Tuple[float, float] = (12, 6),
) -> Figure:
    """Plot step response in time domain.
    
    Args:
        response: Step response samples
        sample_rate: Sample rate in Hz
        title: Plot title
        xlim: Optional time axis limits in seconds
        figsize: Figure size
        
    Returns:
        matplotlib Figure
    """
    fig, ax = plt.subplots(figsize=figsize)
    
    time = np.arange(len(response)) / sample_rate * 1000  # ms
    
    ax.plot(time, response, linewidth=1.2)
    ax.set_xlabel('Time (ms)')
    ax.set_ylabel('Amplitude')
    ax.set_title(title)
    ax.grid(True, alpha=0.3)
    
    if xlim:
        ax.set_xlim(xlim[0] * 1000, xlim[1] * 1000)
    
    # Reference lines
    ax.axhline(1.0, color='green', linestyle='--', alpha=0.5, label='Target')
    ax.axhline(0, color='gray', linewidth=0.5)
    ax.legend()
    
    plt.tight_layout()
    return fig


def plot_thd_bars(
    harmonic_levels: dict,
    thd_percent: float,
    title: str = "Harmonic Distortion",
    figsize: Tuple[float, float] = (10, 6),
) -> Figure:
    """Plot harmonic distortion as bar chart.
    
    Args:
        harmonic_levels: Dict mapping harmonic number to level in dB
        thd_percent: Total THD percentage for display
        title: Plot title
        figsize: Figure size
        
    Returns:
        matplotlib Figure
    """
    fig, ax = plt.subplots(figsize=figsize)
    
    harmonics = list(harmonic_levels.keys())
    levels = list(harmonic_levels.values())
    
    bars = ax.bar(harmonics, levels, color='steelblue', edgecolor='navy')
    
    ax.set_xlabel('Harmonic Number')
    ax.set_ylabel('Level (dB relative to fundamental)')
    ax.set_title(f'{title}\nTHD: {thd_percent:.3f}%')
    ax.set_xticks(harmonics)
    
    ax.axhline(-60, color='red', linestyle='--', alpha=0.5, label='-60 dB')
    ax.axhline(-80, color='orange', linestyle='--', alpha=0.5, label='-80 dB')
    ax.legend()
    
    ax.grid(True, axis='y', alpha=0.3)
    
    plt.tight_layout()
    return fig


def plot_waveform(
    signal_data: np.ndarray,
    sample_rate: int = 44100,
    title: str = "Waveform",
    xlim: Tuple[float, float] | None = None,
    figsize: Tuple[float, float] = (12, 4),
) -> Figure:
    """Plot waveform in time domain.
    
    Args:
        signal_data: Signal samples
        sample_rate: Sample rate in Hz
        title: Plot title
        xlim: Optional time axis limits in seconds
        figsize: Figure size
        
    Returns:
        matplotlib Figure
    """
    fig, ax = plt.subplots(figsize=figsize)
    
    time = np.arange(len(signal_data)) / sample_rate * 1000  # ms
    
    ax.plot(time, signal_data, linewidth=0.5)
    ax.set_xlabel('Time (ms)')
    ax.set_ylabel('Amplitude')
    ax.set_title(title)
    ax.grid(True, alpha=0.3)
    
    if xlim:
        ax.set_xlim(xlim[0] * 1000, xlim[1] * 1000)
    
    ax.axhline(0, color='gray', linewidth=0.5)
    
    plt.tight_layout()
    return fig


def plot_transfer_curve(
    input_levels: np.ndarray,
    output_levels: np.ndarray,
    title: str = "Transfer Curve",
    figsize: Tuple[float, float] = (8, 8),
) -> Figure:
    """Plot input/output transfer curve (for distortion analysis).
    
    Args:
        input_levels: Input amplitude values
        output_levels: Output amplitude values
        title: Plot title
        figsize: Figure size
        
    Returns:
        matplotlib Figure
    """
    fig, ax = plt.subplots(figsize=figsize)
    
    ax.plot(input_levels, output_levels, linewidth=1.5)
    
    # Reference line (linear/unity)
    lim = max(abs(input_levels).max(), abs(output_levels).max())
    ax.plot([-lim, lim], [-lim, lim], 'k--', alpha=0.3, label='Linear')
    
    ax.set_xlabel('Input')
    ax.set_ylabel('Output')
    ax.set_title(title)
    ax.set_aspect('equal')
    ax.grid(True, alpha=0.3)
    ax.legend()
    
    ax.set_xlim(-lim, lim)
    ax.set_ylim(-lim, lim)
    
    plt.tight_layout()
    return fig


def plot_group_delay(
    freqs: np.ndarray,
    delay_samples: np.ndarray,
    sample_rate: int = 44100,
    title: str = "Group Delay",
    xlim: Tuple[float, float] | None = None,
    figsize: Tuple[float, float] = (12, 6),
) -> Figure:
    """Plot group delay vs frequency.
    
    Args:
        freqs: Frequency values in Hz
        delay_samples: Group delay in samples
        sample_rate: Sample rate in Hz
        title: Plot title
        xlim: Optional frequency axis limits
        figsize: Figure size
        
    Returns:
        matplotlib Figure
    """
    fig, ax = plt.subplots(figsize=figsize)
    
    # Convert to milliseconds
    delay_ms = delay_samples / sample_rate * 1000
    
    ax.semilogx(freqs, delay_ms, linewidth=1.2)
    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Group Delay (ms)')
    ax.set_title(title)
    ax.grid(True, which='both', alpha=0.3)
    
    if xlim:
        ax.set_xlim(xlim)
    else:
        ax.set_xlim(20, max(freqs))
    
    plt.tight_layout()
    return fig


def plot_comparison(
    freqs: np.ndarray,
    *signals,
    labels: list[str] | None = None,
    title: str = "Comparison",
    xlim: Tuple[float, float] | None = None,
    ylim: Tuple[float, float] | None = None,
    log_freq: bool = True,
    figsize: Tuple[float, float] = (12, 6),
) -> Figure:
    """Plot multiple spectra for comparison.
    
    Args:
        freqs: Frequency values in Hz
        *signals: Variable number of magnitude arrays (dB)
        labels: Optional labels for each signal
        title: Plot title
        xlim: Optional frequency axis limits
        ylim: Optional magnitude axis limits
        log_freq: If True, use logarithmic frequency axis
        figsize: Figure size
        
    Returns:
        matplotlib Figure
    """
    fig, ax = plt.subplots(figsize=figsize)
    
    if labels is None:
        labels = [f'Signal {i+1}' for i in range(len(signals))]
    
    for signal, label in zip(signals, labels):
        ax.plot(freqs, signal, linewidth=1.0, label=label)
    
    if log_freq:
        ax.set_xscale('log')
    
    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Magnitude (dB)')
    ax.set_title(title)
    ax.grid(True, which='both', alpha=0.3)
    ax.legend()
    
    if xlim:
        ax.set_xlim(xlim)
    else:
        ax.set_xlim(20, freqs[-1])
    
    if ylim:
        ax.set_ylim(ylim)
    
    plt.tight_layout()
    return fig


def plot_aliasing_analysis(
    freqs: np.ndarray,
    magnitude_db: np.ndarray,
    artifacts: list[dict],
    fundamental_freq: float,
    title: str = "Aliasing Analysis",
    figsize: Tuple[float, float] = (14, 6),
) -> Figure:
    """Plot spectrum with aliasing artifacts highlighted.
    
    Args:
        freqs: Frequency values in Hz
        magnitude_db: Magnitude values in dB
        artifacts: List of artifact dicts from find_aliasing()
        fundamental_freq: Fundamental frequency for harmonic reference
        title: Plot title
        figsize: Figure size
        
    Returns:
        matplotlib Figure
    """
    fig, ax = plt.subplots(figsize=figsize)
    
    ax.semilogx(freqs, magnitude_db, linewidth=0.8, label='Spectrum')
    
    # Mark harmonics
    nyquist = freqs[-1]
    h = 1
    while fundamental_freq * h < nyquist:
        ax.axvline(fundamental_freq * h, color='green', alpha=0.3, linewidth=0.5)
        h += 1
    
    # Mark artifacts
    for artifact in artifacts:
        ax.axvline(
            artifact['frequency'],
            color='red',
            alpha=0.7,
            linewidth=2,
            label=f"Alias @ {artifact['frequency']:.1f} Hz ({artifact['level_db']:.1f} dB)"
        )
    
    ax.set_xlabel('Frequency (Hz)')
    ax.set_ylabel('Magnitude (dB)')
    ax.set_title(title)
    ax.grid(True, which='both', alpha=0.3)
    ax.legend(loc='upper right')
    ax.set_xlim(20, nyquist)
    
    plt.tight_layout()
    return fig


def create_dsp_report(
    signal_data: np.ndarray,
    processed_data: np.ndarray,
    sample_rate: int = 44100,
    title: str = "DSP Analysis Report",
    figsize: Tuple[float, float] = (16, 12),
) -> Figure:
    """Create a comprehensive DSP analysis report figure.
    
    Shows waveform comparison, spectrum comparison, and spectrogram.
    
    Args:
        signal_data: Original signal
        processed_data: Processed signal
        sample_rate: Sample rate in Hz
        title: Overall title
        figsize: Figure size
        
    Returns:
        matplotlib Figure
    """
    from . import analysis
    
    fig = plt.figure(figsize=figsize)
    fig.suptitle(title, fontsize=14)
    
    # Create grid
    gs = fig.add_gridspec(3, 2, hspace=0.3, wspace=0.3)
    
    # Waveforms
    ax1 = fig.add_subplot(gs[0, 0])
    ax2 = fig.add_subplot(gs[0, 1])
    
    time = np.arange(min(len(signal_data), 2000)) / sample_rate * 1000
    ax1.plot(time, signal_data[:len(time)], linewidth=0.5)
    ax1.set_title('Input Waveform')
    ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('Amplitude')
    
    ax2.plot(time, processed_data[:len(time)], linewidth=0.5, color='orange')
    ax2.set_title('Output Waveform')
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('Amplitude')
    
    # Spectra
    ax3 = fig.add_subplot(gs[1, :])
    freqs_in, mag_in = analysis.spectrum(signal_data, sample_rate)
    freqs_out, mag_out = analysis.spectrum(processed_data, sample_rate)
    
    ax3.semilogx(freqs_in, mag_in, linewidth=0.8, label='Input', alpha=0.7)
    ax3.semilogx(freqs_out, mag_out, linewidth=0.8, label='Output', alpha=0.7)
    ax3.set_title('Spectrum Comparison')
    ax3.set_xlabel('Frequency (Hz)')
    ax3.set_ylabel('Magnitude (dB)')
    ax3.legend()
    ax3.set_xlim(20, sample_rate / 2)
    ax3.grid(True, which='both', alpha=0.3)
    
    # Spectrograms
    ax4 = fig.add_subplot(gs[2, 0])
    ax5 = fig.add_subplot(gs[2, 1])
    
    times, freqs, Sxx_in = analysis.spectrogram(signal_data, sample_rate)
    _, _, Sxx_out = analysis.spectrogram(processed_data, sample_rate)
    
    vmax = max(Sxx_in.max(), Sxx_out.max())
    vmin = vmax - 80
    
    ax4.pcolormesh(times, freqs, Sxx_in, shading='gouraud', vmin=vmin, vmax=vmax, cmap='magma')
    ax4.set_title('Input Spectrogram')
    ax4.set_xlabel('Time (s)')
    ax4.set_ylabel('Frequency (Hz)')
    
    mesh = ax5.pcolormesh(times, freqs, Sxx_out, shading='gouraud', vmin=vmin, vmax=vmax, cmap='magma')
    ax5.set_title('Output Spectrogram')
    ax5.set_xlabel('Time (s)')
    ax5.set_ylabel('Frequency (Hz)')
    
    plt.colorbar(mesh, ax=[ax4, ax5], label='Power (dB)')
    
    return fig


def save_figure(fig: Figure, path: str, dpi: int = 150):
    """Save figure to file.
    
    Args:
        fig: matplotlib Figure
        path: Output file path
        dpi: Resolution in dots per inch
    """
    fig.savefig(path, dpi=dpi, bbox_inches='tight')
    plt.close(fig)
