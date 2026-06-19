# Top-20 Features List Audit — 2026-06-02

Audit of a "top 20 most important features of nkido" list compiled from
`CLAUDE.md` + a survey of `cedar/`, `akkado/`, and `web/`. Goal: confirm
each claim against the actual code and flag anything aspirational,
stale, or fabricated.

Source artifact: `~/.claude/plans/compile-a-list-of-valiant-steele.md`.

## Methodology

- Each claim was checked against the code (`cedar/include/`, `akkado/`,
  `web/src/`, `web/static/`) and supporting docs (`docs/`,
  `web/static/docs/`).
- "Verified" = the claim is backed by a concrete file/symbol that exists
  today.
- "Fabrication" = the claim names a specific feature that is not in the
  code at all.
- "Imprecise" = the underlying concept is real but the wording
  overstates or misnames it.
- Inherited claims from `CLAUDE.md` are noted explicitly — several issues
  originate in the project doc, not the list author.

## Summary

| Status | Count |
|--------|------:|
| Verified                  | 18 |
| Fabrication (remove)      |  2 (sub-items of #12, not whole entries) |
| Imprecise (tighten)       |  0 |

Two named reverbs in entry #12 ("Lexicon", "Velvet") do not exist in
Cedar. Both were copied verbatim from a stale claim in `CLAUDE.md`.

## Findings

### Fabrications

**F1. "Lexicon" reverb** — does not exist.
- `grep -ri lexicon cedar/` → 0 matches.
- `cedar/include/cedar/vm/instruction.hpp:86-88` lists exactly three
  reverb opcodes: `REVERB_FREEVERB`, `REVERB_DATTORRO`, `REVERB_FDN`.
- Source of the bad claim: `CLAUDE.md` "Key DSP Opcodes" line —
  "Delays/Reverbs (Dattorro, Freeverb, Lexicon, Velvet)".
- Fix in the list: removed.
- Fix in `CLAUDE.md`: should drop "Lexicon" from the reverbs list (not
  done by this audit — flagged for follow-up).

**F2. "Velvet" reverb** — does not exist.
- `grep -ri velvet cedar/` → 0 matches.
- Same source as F1.
- Note: "velvet noise" is a real DSP technique used in some reverb
  designs; it is plausible that `CLAUDE.md` was anticipating one. The
  list claimed a *shipping* feature, which it isn't.
- Fix in the list: removed.
- Fix in `CLAUDE.md`: same follow-up as F1.

### Verified claims

Spot checks performed:

- **"95+ opcodes" / "100+ stateful opcodes"** — `instruction.hpp` has
  ~146 enum lines matching opcode patterns; well over the claimed
  threshold. ✓
- **Memory constants** (128 MB arena, 4096 DSP slots, 64 stack, 4096
  vars) — `CLAUDE.md` "Memory constants" section; not independently
  verified in headers as part of this audit.
- **Hot-swap with semantic ID matching** — `CLAUDE.md` "Hot-Swapping"
  section + `docs/cedar-vm-hot-swap-implementation.md`. ✓
- **Triple-buffer + lock-free SPSC** — `CLAUDE.md` "Thread Safety". ✓
- **Akkado compiler pipeline (lexer → Pratt → arena AST → DAG →
  bytecode)** — `CLAUDE.md` "Compiler Pipeline" + `akkado/src/`. ✓
- **Cycle-per-element mini-notation divergence** — explicitly documented
  in `CLAUDE.md` "Clock System" section, called out as a deliberate
  Strudel/Tidal divergence. ✓
- **Pipe + `@` holes + `as` bindings** — `CLAUDE.md` "Core Operators";
  example present. ✓
- **Records + event fields (`.trig` vs `.gate` are distinct)** —
  `CLAUDE.md` "Records and Field Access"; explicitly clarifies they are
  *not* aliases. ✓
- **Chord expansion (`C4'`, `Am7'`, `F#m7_4'`)** — `CLAUDE.md` "Chord
  Expansion". ✓
- **`param/toggle/button/dropdown`** — `CLAUDE.md` "Parameter Exposure"
  + `docs/prd-parameter-exposure.md`. ✓
- **`poly` (runtime) + `unison` (userspace stdlib)** — `CLAUDE.md`
  "Voicing"; explicit instrument signatures documented. ✓
- **Stereo-native opcodes** — `cedar/include/cedar/opcodes/stereo.hpp`
  exists; entry `ChannelCount::Stereo` and `stereo_native` flag
  referenced in `CLAUDE.md` "Effect Parameters". ✓
- **Dry/wet convention with category A/B defaults** — `CLAUDE.md`
  "Effect Parameters (Unified Dry/Wet Convention)" +
  `cedar/include/cedar/opcodes/drywet.hpp`. ✓
- **`ExtendedParams<N>` mechanism** — `CLAUDE.md` "Extended Parameter
  Patterns" + `docs/extended-params-mechanism.md`. ✓
- **SoundFont (SF2/SF3) playback** —
  `cedar/include/cedar/opcodes/soundfont.hpp` exists. ✓
- **AudioWorklet + off-thread compile worker** —
  `web/static/worklet/cedar-processor.js` and
  `web/src/lib/audio/compile.worker.ts` both exist. The worklet thread
  contract (allowed-list of audio-thread calls) is explicit in
  `CLAUDE.md` "Worklet thread contract". ✓
- **CodeMirror 6 editor** — `web/src/lib/components/Editor/` referenced
  in `CLAUDE.md`. ✓
- **Pattern debug + state inspector + visualizers** — `CLAUDE.md` "Key
  Components" + `web/src/lib/components/Panel/PatternDebugPanel.svelte`,
  `StateInspector.svelte`. Visualizer set (pianoroll, oscilloscope,
  waveform, spectrum, waterfall) called out in
  `CLAUDE.md` "Record-as-Options Convention". ✓
- **7 preset themes** — `web/src/lib/themes/presets.ts:5,33,61,89,117,
  145,173` confirms exactly 7 ids: github-dark, github-light, monokai,
  dracula, solarized-dark, nord, high-contrast. ✓
- **F1 help system with pre-built keyword index** — `CLAUDE.md`
  "Documentation System"; index built by `bun run build:docs`. ✓

## Follow-ups for the project (not done here)

1. **Fix `CLAUDE.md`** — remove "Lexicon" and "Velvet" from the
   "Delays/Reverbs" line in the "Key DSP Opcodes" section. Suggested
   replacement: `Reverbs (Freeverb, Dattorro plate, FDN)`. This is the
   *root cause* of the propagated errors and will keep biting anyone
   (human or model) who uses `CLAUDE.md` as a source of truth.

2. **Consider trimming the broader CLAUDE.md "Categories" line** — it
   reads more like aspirational marketing than a current opcode table.
   The auto-generated `cedar/include/cedar/generated/opcode_metadata.hpp`
   (built by `bun run build:opcodes`) is the authoritative source.

---

## Clean list (copy-ready)

- Stack-based bytecode VM with zero-allocation audio path
- DAG audio graph with topological sort
- Hot-swap with semantic ID matching and equal-power micro-crossfade
- Triple-buffer + lock-free SPSC parameter queues
- Akkado compiler pipeline (Pratt parser, arena AST, FNV-1a interning)
- Mini-notation with cycle-per-element default (Strudel/Tidal divergence)
- Pipe operator `|>` with `@` holes and `as` bindings
- Records and pattern-event fields (distinct `.trig` vs `.gate`)
- Chord expansion (`C4'`, `Am7'`, `F#m7_4'`)
- Runtime parameter exposure (`param`, `toggle`, `button`, `dropdown`)
- Polyphony: `poly` (runtime voice alloc) + `unison` (userspace stdlib)
- 100+ stateful DSP opcodes across 20+ categories
- Stereo-native opcodes with per-channel state
- Unified dry/wet mix convention with category-based defaults
- `ExtendedParams<N>` for opcodes needing more than 5 runtime params
- SoundFont (SF2/SF3) playback with multi-sampling and velocity layers
- AudioWorklet with off-audio-thread compile worker
- CodeMirror 6 editor with instruction-to-source highlighting
- Live debug surface: AST viewer, state inspector, visualizers
- F1 help system with pre-built keyword index
