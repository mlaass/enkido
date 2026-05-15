#!/usr/bin/env python3
"""Generate the .mid test fixtures consumed by cedar/tests/test_midi.cpp.

Run with: ``uv run --with mido python generate.py``

Three files land next to this script:

* ``twinkle.mid``   — format-1, single track, 120 BPM, 24 notes.
* ``tempo-map.mid`` — format-1, single track, four tempo changes inside one
  bar so the ``tempo: file`` vs ``tempo: follow`` branches diverge measurably.
* ``format2.mid``   — format-2 file; cedar's parser must reject it.

All three are tiny (well under 1 KB) and reproducible from this script. We
commit both this generator and the resulting .mid binaries so the tests do
not depend on a Python toolchain at build time.
"""

from pathlib import Path
import mido

OUT = Path(__file__).parent

TPQ = 480


def twinkle() -> mido.MidiFile:
    """Twinkle, Twinkle Little Star — 24 quarter notes at 120 BPM."""
    mid = mido.MidiFile(type=1, ticks_per_beat=TPQ)
    track = mido.MidiTrack()
    mid.tracks.append(track)

    track.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(120), time=0))

    # C4 C4 G4 G4 A4 A4 G4  F4 F4 E4 E4 D4 D4 C4
    # G4 G4 F4 F4 E4 E4 D4
    # G4 G4 F4 F4 E4 E4 D4
    # C4 C4 G4 G4 A4 A4 G4  F4 F4 E4 E4 D4 D4 C4 — but we only want 24 notes.
    notes = [60, 60, 67, 67, 69, 69, 67,
             65, 65, 64, 64, 62, 62, 60,
             67, 67, 65, 65, 64, 64, 62,
             67, 67, 65]

    for n in notes:
        track.append(mido.Message("note_on", note=n, velocity=96, time=0))
        track.append(mido.Message("note_off", note=n, velocity=64, time=TPQ))

    track.append(mido.MetaMessage("end_of_track", time=0))
    return mid


def tempo_map() -> mido.MidiFile:
    """Quarter-note arpeggio with tempo changes — exercises tempo_mode = file.

    Four quarter notes ascending C4-E4-G4-C5; tempo flips before each note so
    follow-mode (engine BPM) and file-mode (embedded tempo map) play back at
    measurably different real-time spans.
    """
    mid = mido.MidiFile(type=1, ticks_per_beat=TPQ)
    track = mido.MidiTrack()
    mid.tracks.append(track)

    notes = [60, 64, 67, 72]
    tempos_bpm = [120, 60, 180, 90]

    accumulated_dt = 0
    for note, bpm in zip(notes, tempos_bpm):
        track.append(mido.MetaMessage("set_tempo",
                                      tempo=mido.bpm2tempo(bpm),
                                      time=accumulated_dt))
        track.append(mido.Message("note_on", note=note, velocity=100, time=0))
        track.append(mido.Message("note_off", note=note, velocity=64, time=TPQ))
        accumulated_dt = 0

    track.append(mido.MetaMessage("end_of_track", time=0))
    return mid


def format2() -> mido.MidiFile:
    """One-pattern format-2 file. Cedar's parser must return nullptr."""
    mid = mido.MidiFile(type=2, ticks_per_beat=TPQ)
    track = mido.MidiTrack()
    mid.tracks.append(track)
    track.append(mido.Message("note_on", note=60, velocity=100, time=0))
    track.append(mido.Message("note_off", note=60, velocity=64, time=TPQ))
    track.append(mido.MetaMessage("end_of_track", time=0))
    return mid


def main() -> None:
    twinkle().save(OUT / "twinkle.mid")
    tempo_map().save(OUT / "tempo-map.mid")
    format2().save(OUT / "format2.mid")
    for path in sorted(OUT.glob("*.mid")):
        size = path.stat().st_size
        print(f"  {path.name}: {size} bytes")


if __name__ == "__main__":
    main()
