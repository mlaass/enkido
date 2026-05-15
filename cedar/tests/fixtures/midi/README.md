# MIDI test fixtures

Reproducible from `generate.py`:

```bash
uv run --with mido python cedar/tests/fixtures/midi/generate.py
```

| File | Format | Purpose |
|---|---|---|
| `twinkle.mid` | 1 | 24 quarter notes at 120 BPM, exercises basic note-on / note-off / EOT |
| `tempo-map.mid` | 1 | Four notes with tempo changes (120 → 60 → 180 → 90 BPM), exercises tempo-map honouring in `tempo: "file"` mode |
| `format2.mid` | 2 | Single-track format-2 file; `parse_smf` must reject and return `nullptr` |

The binaries are committed because the C++ test harness cannot run Python at
build time; the generator stays alongside so future changes are auditable.
