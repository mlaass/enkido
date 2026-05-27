# Contributing to NKIDO

Thanks for your interest in contributing to NKIDO! This guide covers everything you need to get started — from reporting bugs to adding new DSP opcodes.

## Code of Conduct

By participating in this project you agree to follow the [Hippocratic Code of Conduct](CODE_OF_CONDUCT.md). The short version: first, do no harm. Be polite, criticize the code rather than the coder, and assume good faith. If you experience or witness unacceptable behavior, open a GitHub issue or contact the maintainers as described in the [Code of Conduct](CODE_OF_CONDUCT.md#v-resolution-and-enforcement).

## Getting Started

### Requirements

- C++20 compiler (GCC 10+, Clang 10+, MSVC 2019+)
- CMake 3.21+
- [Bun](https://bun.sh/) (for web app and code generation scripts)
- [Emscripten](https://emscripten.org/) (only for WASM builds)
- Python 3.10+ with [uv](https://github.com/astral-sh/uv) (for DSP experiments)

### Building from Source

```bash
# Clone your fork
git clone https://github.com/<your-username>/nkido.git
cd nkido

# Configure and build (debug)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Run tests
./build/cedar/tests/cedar_tests
./build/akkado/tests/akkado_tests
```

See [README.md](README.md) for build presets and additional targets.

## How to Contribute

### Reporting Bugs

Open a [GitHub issue](https://github.com/mlaass/nkido/issues) with:

- A clear title describing the problem
- Steps to reproduce (include Akkado code snippets if applicable)
- Expected vs. actual behavior
- Your platform and compiler version
- For audio bugs: attach a short WAV recording if possible — ears catch what assertions miss

### Submitting Changes

1. **Fork** the repository and create a feature branch from `master`
2. Make your changes (see conventions below)
3. Run the test suite and make sure it passes
4. **Open a pull request** against `master`
5. Describe what your PR does and why — link related issues

Keep PRs focused on a single concern. A bug fix and a new feature should be separate PRs.

### What to Work On

All contributions are welcome. Here are some ways to help:

- **Fix bugs** — check the issue tracker for open bugs
- **Improve documentation** — better explanations, more examples, fix typos
- **Add or optimize opcodes** — new DSP algorithms or performance improvements to existing ones
- **Improve platform support** — testing on different OS/compiler combinations, fixing portability issues
- **Web IDE improvements** — UI/UX enhancements, new visualizations
- **Write DSP experiments** — Python test scripts that verify opcode behavior (see below)

## Project Structure

```
nkido/
├── cedar/          # Audio synthesis engine (standalone C++ library)
├── akkado/         # Language compiler (depends on cedar)
├── tools/          # CLI tools (nkido, akkado)
├── web/            # SvelteKit web IDE
├── experiments/    # Python DSP test scripts
└── docs/           # Design documents and PRDs
```

**Cedar** is the low-level audio engine — it knows nothing about the Akkado language. **Akkado** is the compiler that translates Akkado source into Cedar bytecode. This separation is intentional; keep it clean.

## Code Conventions

### C++ Style

The project uses `.clang-format` (Google base, 4-space indent, 120-column limit). Format your code before committing:

```bash
clang-format -i path/to/file.cpp
```

### Naming

- Types and classes: `PascalCase`
- Functions and methods: `snake_case`
- Constants and enum values: `UPPER_SNAKE_CASE`
- Member variables: `snake_case` (no prefix)
- Namespaces: `cedar`, `akkado` (lowercase)

### Real-Time Audio Path Rules

Code that runs in the audio thread (Cedar VM, opcode implementations) must follow strict rules:

- **Zero heap allocations** — no `new`, `malloc`, `std::vector::push_back`, or anything that can allocate. Use pre-allocated pools and fixed-size arrays.
- **No locks** — no mutexes, no condition variables. Use lock-free structures (triple buffers, SPSC queues, atomic swaps).
- **No syscalls** — no file I/O, no logging, no exceptions in the hot path.
- **Block processing** — always process 128 samples at a time (`BLOCK_SIZE`). Never process sample-by-sample in a loop that could be vectorized.

### Memory and Data Structures

- **Arena allocation with indices** — use arena-allocated structures with integer indices instead of raw pointers. This improves cache locality and simplifies memory management.
- **String interning** — identifiers are interned with FNV-1a hashing. Compare by hash, not by string content.
- **Pre-allocated state pools** — stateful opcodes (oscillators, filters, delays) store their state in `StatePool` using a `state_id`. Never allocate state dynamically.

### Performance Hints

- Use `[[likely]]` / `[[unlikely]]` in the VM dispatch switch
- Prefer SIMD intrinsics (SSE/AVX) for hot inner loops when the speedup is measurable
- Profile before optimizing — don't guess where the bottleneck is

### Thread Safety

Communication between the compiler thread and audio thread uses:
- **Triple buffering** — compiler writes to "Next", audio reads from "Current"
- **Lock-free SPSC queues** — for parameter updates
- **Atomic pointer swap** — at block boundaries, never mid-block

### Web App

The web app uses SvelteKit with Svelte 5 runes. Always use `bun` (not npm). Contributions to the web app should follow the existing store pattern in `web/src/lib/stores/`.

## Adding a New Opcode

This is one of the most impactful ways to contribute. The full process:

### 1. Implement the Opcode in Cedar

Add your opcode to the appropriate file in `cedar/include/cedar/opcodes/`. Choose the right category (e.g., `filters.hpp`, `oscillators.hpp`, `delays.hpp`).

Each opcode is a case in the VM dispatch switch. It reads inputs from buffers, processes a block of 128 samples, and writes to an output buffer:

```cpp
case Opcode::MY_NEW_OP: {
    auto& state = ctx.states->get_or_create<MyState>(inst.state_id);
    const float* in = ctx.get_input(inst, 0);
    float* out = ctx.get_output(inst);
    for (std::size_t i = 0; i < BLOCK_SIZE; ++i) {
        out[i] = /* your DSP here */;
    }
    break;
}
```

Add the opcode name to the `Opcode` enum in `cedar/include/cedar/vm/instruction.hpp`.

### 2. Register the Builtin in Akkado

Add an entry to the `BUILTIN_FUNCTIONS` map in `akkado/include/akkado/builtins.hpp`:

```cpp
{"my_op", {cedar::Opcode::MY_NEW_OP, 2, 1, true,
           {"in", "freq", "res", "", "", ""},
           {0.7f, NAN, NAN, NAN, NAN},
           "Brief description of what it does"}},
```

Fields: opcode, required input count, optional input count, requires state, parameter names, default values, description.

### 3. Regenerate Opcode Metadata

```bash
cd web && bun run build:opcodes
```

This updates `cedar/include/cedar/generated/opcode_metadata.hpp`.

### 4. Write Tests

Add Catch2 tests in the appropriate test file under `cedar/tests/` or `akkado/tests/`:

```bash
# Run your tests
./build/akkado/tests/akkado_tests "my_op*"
```

### 5. Write a Python DSP Experiment

Create `experiments/test_op_<name>.py` following the project's experiment methodology. This provides human-verifiable WAV output alongside automated assertions:

```python
from cedar_testing import CedarTestHost, output_dir

OUT = output_dir("op_my_op")

def test_basic():
    """Test MY_OP for expected frequency response."""
    host = CedarTestHost()
    # ... set up instructions, process blocks ...

    # Always save WAV for human evaluation
    wav_path = os.path.join(OUT, "test_basic.wav")
    scipy.io.wavfile.write(wav_path, host.sr, output)
    print(f"  Saved {wav_path} - Listen for [specific thing]")

    if meets_criteria:
        print("  ✓ PASS: description")
    else:
        print("  ✗ FAIL: description")

if __name__ == "__main__":
    test_basic()
```

Run experiments with:

```bash
cd experiments
uv run python test_op_my_op.py
```

### 6. Add Documentation

Add your opcode to the relevant user-facing docs in `web/static/docs/`, then rebuild the docs index:

```bash
cd web && bun run build:docs
```

### 7. Update the Checklist

Update `docs/dsp-quality-checklist.md` to reflect your new opcode's test coverage.

## Testing

Before submitting a PR, make sure tests pass:

```bash
# C++ tests
./build/cedar/tests/cedar_tests
./build/akkado/tests/akkado_tests

# Python DSP experiments (if you changed opcodes)
cd experiments && ./run_all.sh

# Web app type checking (if you changed web code)
cd web && bun run check
```

If a test fails, investigate — don't adjust thresholds or expected values to make it pass. If you believe the test itself is wrong, explain why in your PR.

## Releasing

NKIDO follows [Semantic Versioning](https://semver.org/). The release flow is:

1. **Update the changelog.** Run the `/update-changelog` Claude Code skill (or hand-edit `CHANGELOG.md`) to add a `## [X.Y.Z] - YYYY-MM-DD` section in [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) format. The skill drafts entries from `git log` since the last tag.
2. **Review and commit.** Inspect `git diff CHANGELOG.md`, then commit the entry on its own.
3. **Bump the version.** Run `./scripts/bump-version.sh <major|minor|patch>`. This syncs `VERSION` and `web/package.json`, commits with `Release vX.Y.Z`, and creates the `vX.Y.Z` tag. The script aborts if `CHANGELOG.md` has no section for the new version.
4. **Push.** `git push origin master --tags` triggers `.github/workflows/deploy.yml`, which deploys to production Netlify and creates a GitHub Release whose body is extracted from `CHANGELOG.md` by `scripts/extract-changelog.sh`.

`CHANGELOG.md` is the single source of truth for release notes — anything you put there shows up on the GitHub Release page.

## Questions?

Open a [discussion](https://github.com/mlaass/nkido/discussions) or ask in a GitHub issue. We're happy to help you find the right place to contribute.
