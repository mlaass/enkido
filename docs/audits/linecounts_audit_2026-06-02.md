# Project Line Count Audit — 2026-06-02

Snapshot of source-line counts across the nkido repo, broken down by
**project part** and **language**. Useful for sizing work, comparing
the relative weight of subsystems, and spotting which areas have
ballooned (or stayed lean) over time.

## Methodology

- Counted with a small walker (`scripts/_linecount.py`) that classifies
  files by extension and by the directory prefix listed in `CLAUDE.md`'s
  "Project Structure" section.
- **Raw line counts** (not SLOC). Blank lines and comments are included.
  Treat the numbers as relative-size signal, not as effort estimates.
- **Excluded:** `build/`, `build-cedar/`, `node_modules/`,
  `.svelte-kit/`, `dist/`, `__pycache__/`, `output/`, the `.git`
  directory, anything under a `generated/` path, and `*.generated.ts`.
- **Included:** `cedar/third_party/` is broken out as its own row so
  it's visible but doesn't inflate "our" cedar count.
- `web/static/docs/` (user-facing docs shipped with the web app) is
  separate from `docs/` (internal/design docs).
- Tests are split out from production code for `cedar` and `akkado`.

## Summary by subsystem

Cedar includes `cedar/third_party/` (~35k lines of vendored single-header
libs). Web bundles everything under `web/` except `web/tests/` and
`web/share-api/test/`. "Other" covers `docs/`, `experiments/`, `tools/`,
`scripts/`, `cmake/`, and root files.

| Group | Files | Lines | % |
|-------|------:|------:|--:|
| akkado | 75 | 39,448 | 12.3% |
| akkado tests | 55 | 43,172 | 13.4% |
| cedar | 105 | 58,054 | 18.1% |
| cedar tests | 38 | 16,920 | 5.3% |
| web | 211 | 49,304 | 15.4% |
| web tests | 16 | 2,637 | 0.8% |
| other | 339 | 111,646 | 34.8% |
| **TOTAL** | **839** | **321,181** | 100% |

## Summary by language

| Language | Files | Lines | % |
|----------|------:|------:|--:|
| C++ | 289 | 160,344 | 49.9% |
| Markdown | 240 | 81,626 | 25.4% |
| Python | 119 | 31,959 | 10.0% |
| TypeScript | 104 | 22,080 | 6.9% |
| Svelte | 40 | 9,770 | 3.0% |
| C | 3 | 6,163 | 1.9% |
| JSON | 11 | 4,695 | 1.5% |
| JavaScript | 2 | 1,621 | 0.5% |
| CMake | 15 | 1,417 | 0.4% |
| Shell | 10 | 1,069 | 0.3% |
| CSS | 1 | 314 | 0.1% |
| TOML | 4 | 103 | 0.0% |
| HTML | 1 | 20 | 0.0% |
| **TOTAL** | **839** | **321,181** | 100% |

## Detailed breakdown (part × language)

| Part | Language | Files | Lines |
|------|----------|------:|------:|
| docs | Markdown | 163 | 68,679 |
| akkado (tests) | C++ | 54 | 43,083 |
| akkado | C++ | 74 | 39,287 |
| experiments | Python | 102 | 30,300 |
| cedar (third_party) | C++ | 9 | 28,774 |
| cedar | C++ | 90 | 22,856 |
| cedar (tests) | C++ | 35 | 16,726 |
| web (src) | TypeScript | 68 | 16,626 |
| web (static docs) | Markdown | 63 | 10,570 |
| web (src) | Svelte | 40 | 9,770 |
| tools | C++ | 26 | 7,423 |
| cedar (third_party) | C | 3 | 6,163 |
| web (static) | JSON | 3 | 4,292 |
| web (tests) | TypeScript | 16 | 2,637 |
| web (wasm bindings) | C++ | 1 | 2,195 |
| web (build scripts) | TypeScript | 7 | 1,677 |
| root | Markdown | 7 | 1,614 |
| web (worklet) | JavaScript | 1 | 1,596 |
| web (config) | TypeScript | 13 | 1,140 |
| tools | Python | 13 | 1,061 |
| scripts | Shell | 6 | 917 |
| scripts | Python | 3 | 499 |
| web (config) | Markdown | 2 | 438 |
| web (src) | CSS | 1 | 314 |
| cmake | CMake | 4 | 287 |
| web (wasm bindings) | CMake | 1 | 263 |
| cedar | CMake | 2 | 249 |
| tools | CMake | 4 | 222 |
| experiments | Markdown | 2 | 208 |
| root | JSON | 2 | 165 |
| akkado | CMake | 1 | 161 |
| web (static patches) | JSON | 1 | 125 |
| web (build scripts) | Shell | 1 | 110 |
| web (config) | JSON | 4 | 101 |
| cedar (tests) | Python | 1 | 99 |
| akkado (tests) | CMake | 1 | 89 |
| tools | Markdown | 1 | 89 |
| cedar (tests) | CMake | 1 | 79 |
| root | CMake | 1 | 67 |
| experiments | Shell | 1 | 38 |
| web (config) | TOML | 1 | 38 |
| root | TOML | 1 | 27 |
| tools | TOML | 1 | 27 |
| web (config) | JavaScript | 1 | 25 |
| web (src) | HTML | 1 | 20 |
| cedar (tests) | Markdown | 1 | 16 |
| cedar (third_party) | Markdown | 1 | 12 |
| tools | JSON | 1 | 12 |
| experiments | TOML | 1 | 11 |
| web (wasm bindings) | Shell | 2 | 4 |

## Observations

- **C++ dominates at 50%** of total lines. ~29k of that (≈18% of all
  C++) is vendored single-header libs in `cedar/third_party/`
  (`dr_flac.h`, `httplib.h`, `kissfft`, `minimp3`, `stb_vorbis`,
  `tsf.h`). First-party C++ is closer to 125k.
- **Tests outweigh production code in akkado** (43k tests vs 39k src) —
  a sign that the language surface is well-covered. Cedar tests are
  ~73% of cedar src (17k vs 23k).
- **Markdown is the second-largest language** (81k lines, 25%). Most of
  that is `docs/` (68k) which is design docs / PRDs / audits, plus
  `web/static/docs/` (11k) which is user-facing reference shipped with
  the web app. Worth checking whether any of `docs/` is stale before
  the next big refactor.
- **`experiments/` is a meaningful 9.5%** (30k lines of Python). These
  are per-opcode test/visualization scripts — large because there's one
  per opcode and they often include reference plots.
- **Web frontend is compact** at ~27k lines TS+Svelte combined, despite
  the breadth of UI features (editor, transport, panels, theming, docs,
  pattern viz). State-management overhead is light thanks to runes.

## Regenerating

```bash
python scripts/_linecount.py
```

The script lives at `scripts/_linecount.py` and is self-contained
(stdlib only). Adjust the `lang_by_ext` map or `get_part()` if you want
to slice differently.
