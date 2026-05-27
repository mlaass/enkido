# PRD: cedar-esp32 — Cedar Audio Synthesis on ESP32 (AI-Thinker A1S)

> **Status: SHIPPED (external)** — Implemented in sibling repository `~/workspace/cedar-esp32`. This PRD is retained here as the original design doc; all further work and documentation lives in that repo.

---

## 1. Context

Nkido's `cedar/` library is a zero-allocation, host-API-agnostic audio synthesis engine. Commit `c9701a4` added an `esp32` CMake preset and `minsize-stripped` variant that tighten memory limits (`ARENA_SIZE=262144`, `MAX_BUFFERS=64`, `MAX_STATES=128`, `MAX_VARS=512`, `MAX_PROGRAM_SIZE=1024`, `FLOAT_ONLY=ON`) and disable optional modules (audio decoders, SoundFont, FFT, file I/O, MinBLEP). Built with the host compiler, the stripped archive is ~146 KB (see `scripts/cedar-size-report.sh` and `docs/cedar-size-report.md`). This is a size-optimized *profile*, not yet a cross-compiled build: no xtensa toolchain is configured in this repo, so the profile demonstrates the memory/feature shape but has not actually been compiled for xtensa-esp32. Establishing that cross-compile is the first task of cedar-esp32.

**What is missing** to actually run on hardware: an ESP-IDF project skeleton. There is no `idf_component.yml`, no `main/`, no `sdkconfig.defaults`, no partition table, no audio I/O driver, no bytecode loader — and no xtensa toolchain integration.

This PRD proposes a new sibling repository, **`cedar-esp32`**, that provides the ESP-IDF project glue, ESP-ADF integration, and board-support code needed to boot an AI-Thinker ESP32-A1S Audio Kit 2.2 and play Cedar bytecode out of the onboard ES8388 codec. The nkido repo is consumed as a git submodule so Cedar stays authoritative in one place.

**Desired outcome:** `idf.py -p /dev/ttyUSB0 flash monitor` on a stock A1S board plays a hardcoded Cedar program out of the headphone jack. A host-side tool (`cedar-push`) uploads new bytecode over UART for hot-swap.

---

## 2. Problem Statement

| Today | With cedar-esp32 |
|---|---|
| `cmake --preset esp32` builds a static lib; no runtime. | `idf.py build` produces flashable firmware. |
| No audio driver; Cedar's `VM::process_block(L, R)` needs host-supplied output buffers. | ESP-ADF pipeline feeds I2S → ES8388 → headphone/speaker. Live external input (`in()`) depends on [`prd-audio-input.md`](prd-audio-input.md) landing; until it does, cedar-esp32 is output-only. |
| No way to run user code on the device. | UART bytecode loader + A1S onboard buttons for live control. |
| Board-specific pins/clocks live nowhere. | `sdkconfig.defaults` + pinned ADF fork carry A1S 2.2 board config. |
| Samples can't be decoded on-device. | ESP-ADF decoders (WAV/MP3/FLAC) feed raw PCM to Cedar samplers. |

---

## 3. Goals and Non-Goals

### Goals

1. **Flashable firmware**: `idf.py build flash monitor` from a clean checkout produces a working A1S 2.2 binary.
2. **Audio out**: ES8388 codec drives the headphone jack at 48 kHz / 128-sample blocks (matches the project-wide default in `CLAUDE.md`; menuconfig-tunable).
3. **Hardcoded demo**: Boots, runs an embedded Cedar bytecode blob, plays audio immediately.
4. **Bytecode upload over UART**: Host-side `cedar-push` tool streams new bytecode; the device calls `VM::load_program()`, which queues the swap for the next block boundary. Cedar's built-in crossfade (3 blocks default, configurable 2–5 via `set_crossfade_blocks()`) handles the fade — ≈8 ms at 48 kHz / 128.
5. **Sample decode via ESP-ADF**: WAV/MP3/FLAC decoded by ADF pipeline → raw PCM buffers Cedar samplers consume by ID.
6. **Hardware controls**: A1S onboard keys (KEY1–KEY6) mapped to Akkado `param()`/`toggle()`/`button()` slots. External pot support over ADC documented but optional.
7. **Build reproducibility**: `README` documents both local ESP-IDF install and `espressif/idf` Docker workflow. Either produces byte-identical output at the same SHAs.
8. **Size fit**: Firmware fits in standard 4 MB flash; PSRAM used freely for sample buffers and Cedar arena. _Actual xtensa binary size is a Phase 1 deliverable — the ~146 KB host figure does not include xtensa code expansion, ESP-IDF/FreeRTOS baseline, ESP-ADF, decoders, or the app layer._

### Non-Goals

- **On-device Akkado compilation.** Akkado's ~36k LOC of compiler code stays host-only. Device receives pre-compiled bytecode.
- **OTA firmware updates.** Flashing is over USB/UART only. OTA is future work.
- **MIDI input** (USB-MIDI, BLE-MIDI, or serial MIDI). Future work.
- **WiFi / web editor integration.** Phase 3 work (see §8); not in MVP.
- **SD card sample loading.** Phase 2 (see §8); UART is the only transport in Phase 1.
- **ESP32-C3 support.** Earlier scoping considered C3, but the A1S is ESP32 classic (LX6). A C3 port is deferred until the A1S build is proven.

---

## 4. Target Experience

### 4.1 First-time build (local ESP-IDF)

```bash
git clone --recurse-submodules git@github.com:<user>/cedar-esp32.git
cd cedar-esp32
. $IDF_PATH/export.sh            # user-installed ESP-IDF v5.x
idf.py set-target esp32
idf.py menuconfig                # optional: tweak SR, block size
idf.py -p /dev/ttyUSB0 flash monitor
# → boots, prints "cedar-esp32 ready, SR=48000 block=128"
# → plays hardcoded demo tone
```

### 4.2 First-time build (Docker)

```bash
git clone --recurse-submodules git@github.com:<user>/cedar-esp32.git
cd cedar-esp32
./scripts/docker-build.sh        # wraps espressif/idf:v5.x
./scripts/docker-flash.sh /dev/ttyUSB0
idf.py -p /dev/ttyUSB0 monitor   # monitoring still uses host idf.py or picocom
```

### 4.3 Live bytecode push

```bash
# Compile on host using existing akkado
./build/bin/akkado compile patch.akk -o patch.cbc

# Push to device
cedar-push --port /dev/ttyUSB0 patch.cbc
# → device logs: "swap: 342 bytes, 12 ops, xfade 10ms"
# → audio morphs to the new patch
```

### 4.4 Hardware controls (A1S KEY1–KEY6)

Akkado source:

```akkado
cutoff = param("cutoff", 800, 100, 8000)   // mapped to KEY1 (step ±100 per press)
hit    = button("trigger")                  // KEY2 momentary
mute   = toggle("mute", false)              // KEY3 latching
osc("saw", 110) |> filter_lp(@, cutoff, 0.7, 1.0, 1.0) |> out(@, @)
```

Key mapping is compiled into the bytecode's parameter metadata (first N `param`/`toggle`/`button` declarations → KEY1..KEY6 in declaration order). Overflow declarations are unreachable on device.

---

## 5. Architecture

### 5.1 System diagram

```
┌─────────────────┐  UART   ┌─────────────────────────────────────────────┐
│  Host           │ ──────► │  ESP32-A1S                                  │
│  - akkado   │         │  ┌──────────────────────────────────────┐   │
│  - cedar-push   │         │  │  main/ (cedar-esp32 app)             │   │
└─────────────────┘         │  │  ├─ boot: init ADF + Cedar           │   │
                            │  │  ├─ UART listener (bytecode loader)  │   │
                            │  │  ├─ key scanner (KEY1..KEY6 → params)│   │
                            │  │  └─ audio task (core 1, high prio)   │   │
                            │  └────────────┬─────────────────────────┘   │
                            │               │                             │
                            │  ┌────────────▼────────────┐                │
                            │  │  ESP-ADF pipeline        │                │
                            │  │  [sample files] → decode │                │
                            │  │  → PCM ring buffer       │                │
                            │  │  → raw_stream OUT        │                │
                            │  └────────────┬────────────┘                │
                            │               │ PCM in (optional)           │
                            │  ┌────────────▼────────────┐                │
                            │  │  Cedar VM (IDF component)│                │
                            │  │  - process_block(L,R)    │                │
                            │  │  - A/B channel + xfade   │                │
                            │  │  - sample refs → ADF buf │                │
                            │  └────────────┬────────────┘                │
                            │               │ stereo PCM out              │
                            │  ┌────────────▼────────────┐                │
                            │  │  I2S → ES8388 codec      │                │
                            │  │  → headphone / speakers  │                │
                            │  └─────────────────────────┘                │
                            └─────────────────────────────────────────────┘
```

### 5.2 Component layout

```
cedar-esp32/
├── CMakeLists.txt                    # top-level ESP-IDF project
├── sdkconfig.defaults                # A1S pins, PSRAM, FreeRTOS tick
├── sdkconfig.defaults.esp32          # chip-specific overrides
├── partitions.csv                    # app / nvs / data partitions
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                        # app_main entry, task setup
│   ├── audio_task.c                  # core-1 audio render loop
│   ├── uart_loader.c                 # bytecode upload protocol
│   ├── key_scanner.c                 # KEY1..6 → param slots
│   ├── adf_pipeline.c                # ADF sample decode → PCM buffers
│   ├── cedar_host.cpp                # C++ glue to Cedar VM
│   └── demo_bytecode.h               # embedded hardcoded program
├── components/
│   ├── cedar/                        # IDF component wrapping nkido/cedar
│   │   ├── CMakeLists.txt
│   │   └── idf_component.yml
│   └── cedar-esp32-hal/              # board helpers (key debouncer, ADC)
├── third_party/
│   ├── nkido/                       # git submodule → nkido repo, pinned SHA
│   └── esp-adf/                      # git submodule → donny681/esp-adf
├── tools/
│   └── cedar-push/                   # Python host tool for UART upload
│       ├── cedar_push/__main__.py
│       └── pyproject.toml
├── scripts/
│   ├── docker-build.sh
│   └── docker-flash.sh
├── docs/
│   ├── PRD.md                        # this document
│   ├── uart-protocol.md              # wire format spec
│   ├── a1s-pinout.md                 # board pin reference
│   └── build-docker.md
├── docker/
│   └── Dockerfile                    # thin wrapper over espressif/idf
└── README.md
```

### 5.3 UART protocol (bytecode loader)

Minimal framed protocol on the existing USB-UART (same port used for monitor). Device listens on a dedicated UART task; `monitor` output is unaffected because frames use a magic-byte escape.

```
Frame: [MAGIC=0xCE 0xDA][LEN u32 LE][TYPE u8][PAYLOAD LEN bytes][CRC32 LE]

TYPE:
  0x01  SWAP_BYTECODE    payload = Cedar bytecode blob
  0x02  SET_PARAM        payload = [slot u8][value f32 LE]
  0x03  PING             payload = empty
  0x80  ACK              (device → host) last-frame-ok
  0x81  NACK             (device → host) [reason u8]
```

Host tool `cedar-push` sends `SWAP_BYTECODE`, waits for `ACK`, exits. No streaming — whole blob fits in a single frame (size bounded by `MAX_PROGRAM_SIZE=1024` opcodes ≈ 16 KB).

**Operational parameters** (full details in `docs/uart-protocol.md`):
- **Baud**: 460800 default (shared with `idf.py monitor`). Configurable via menuconfig.
- **Max LEN**: the loader hard-caps LEN at `MAX_PROGRAM_SIZE * sizeof(Instruction) + header overhead` (~16.5 KB); oversized frames are rejected with `NACK[PROGRAM_TOO_LARGE]` without allocating.
- **Resync**: per-frame timeout of 2 s from magic-byte to CRC. On timeout, bad CRC, or garbage LEN, the parser drops back to scanning for the next `0xCE 0xDA` sequence — a torn transmission never permanently wedges the loader.

### 5.4 Memory budget (PSRAM variant, typical A1S)

| Region | Size | Location | Notes |
|---|---|---|---|
| Cedar arena | 256 KB | PSRAM | `CEDAR_ARENA_SIZE=262144` from esp32 preset. PSRAM chosen because internal SRAM is already claimed by IDF/ADF baseline, VM state, and DMA buffers; arena access is per-block (128 frames) which amortizes PSRAM latency over cache lines. |
| Sample PCM buffers | ≤2 MB | PSRAM | ADF decoders fill; Cedar samplers reference |
| Cedar VM state (stack, vars, states) | ~40 KB | Internal SRAM | Tight real-time access |
| I2S DMA buffers | 4×1024 frames | Internal SRAM | ADF default |
| FreeRTOS + IDF + ADF | ~180 KB | Internal SRAM | Baseline |
| Firmware binary | ≤1.5 MB | Flash | Leaves headroom on 4 MB flash |

Non-PSRAM fallback: reduce arena to 64 KB, disable sample decoding. Phase-1 firmware detects PSRAM at boot and logs a warning if absent.

### 5.5 Hot-swap semantics

The UART task calls `VM::load_program(std::span<const Instruction>)` (see `cedar/include/cedar/vm/vm.hpp:54`), which returns a `LoadResult` and queues the program for swap at the next block boundary. Cedar's internal A/B slot + crossfade (`set_crossfade_blocks()`, 2–5 blocks, default 3 ≈ 8 ms at 48 kHz / 128) handles the atomic pointer flip and fade. No new synchronization primitives are needed on the device — this path is already exercised by the desktop/web runtimes.

### 5.6 Dependencies

| Dependency | Required version | Notes |
|---|---|---|
| ESP-IDF | ≥ 5.1 (tested on 5.x release branch) | Declared in `components/cedar/idf_component.yml`. |
| xtensa-esp-elf toolchain | Bundled with ESP-IDF install | Installed by `$IDF_PATH/install.sh`. |
| ESP-ADF | `donny681/esp-adf`, pinned by submodule tag | Community fork with A1S 2.2 board support. Mainline ADF lacks A1S 2.2 config — migration tracked as OQ#3. |
| Python | ≥ 3.9 (for `cedar-push`) | Uses `pyserial`; installed via `uv tool install .`. |
| Docker (optional) | ≥ 24.x | Only for the `espressif/idf:release-v5.x` workflow. |
| Hardware | AI-Thinker ESP32-A1S Audio Kit 2.2 with PSRAM | Non-PSRAM variants listed as future work (§8). |

---

## 6. Impact Assessment

| Component | Status | Notes |
|---|---|---|
| `cedar/` library code | **Stays** | No API changes expected. Already builds for xtensa-esp32 via `esp32` preset. |
| `cedar/CMakeLists.txt` | **Stays** | Existing `CEDAR_*` size options are used as-is. |
| `scripts/cedar-size-report.sh` | **Stays** | Size report continues to validate the embedded config. |
| nkido `cmake/` | **Stays** | `CompilerOptions.cmake` MinSizeRel flags work inside ESP-IDF. |
| `akkado/` compiler | **Stays** | Host-only; no changes. |
| `tools/akkado/` | **Stays** | Produces bytecode consumed verbatim by the device. |
| New repo `cedar-esp32` | **New** | Contains ESP-IDF project, ADF integration, loader, hardware glue. |
| `tools/cedar-push` (host) | **New** | Python CLI for UART bytecode upload. Lives in cedar-esp32 repo. |
| **[OPEN QUESTION]** Cedar API for sample registration by ID | **Possibly modified** | If Cedar samplers currently expect Cedar to own sample decoding, they need a hook to reference externally-decoded PCM. To be confirmed during Phase 2 when ADF integration begins. |

---

## 7. File-Level Changes (new repo)

All paths are under `cedar-esp32/` (the new sibling repo).

| File | Change |
|---|---|
| `CMakeLists.txt` | New — top-level `idf_component_register` setup, includes `components/cedar`. |
| `sdkconfig.defaults` | New — A1S-appropriate defaults: PSRAM enabled, CPU 240 MHz, I2S pin config for ES8388. |
| `partitions.csv` | New — standard single-app 4 MB layout with NVS partition. |
| `main/main.c` | New — `app_main` boots ADF, creates audio task pinned to core 1. |
| `main/audio_task.c` | New — render loop: pull I2S buffers, call Cedar `process_block`, push to codec. |
| `main/uart_loader.c` | New — UART frame parser, CRC check, A/B bytecode swap. |
| `main/key_scanner.c` | New — debounce KEY1..6, write to Cedar param slots. |
| `main/adf_pipeline.c` | New — ADF pipeline factory for WAV/MP3/FLAC → raw PCM buffers. |
| `main/cedar_host.cpp` | New — C++ glue (Cedar is C++; IDF is mostly C). |
| `main/demo_bytecode.h` | New — `const uint8_t demo_bc[]` generated from a sample `.akk` file at build time. |
| `components/cedar/CMakeLists.txt` | New — wraps nkido submodule as an IDF component; passes `-DCEDAR_*` size flags. |
| `components/cedar/idf_component.yml` | New — declares dependency on IDF ≥ 5.1. |
| `components/cedar-esp32-hal/*` | New — thin helpers (GPIO debounce, optional ADC pot reader). |
| `third_party/nkido/` | New — git submodule pointing at nkido at a known SHA. |
| `third_party/esp-adf/` | New — git submodule pointing at donny681/esp-adf tag. |
| `tools/cedar-push/` | New — Python tool, `pyproject.toml`, published nowhere; `uv tool install .` locally. |
| `docker/Dockerfile` | New — inherits `espressif/idf:release-v5.x`, adds Python deps for cedar-push. |
| `scripts/docker-build.sh` | New — `docker run --rm -v $PWD:/project espressif/idf idf.py build`. |
| `scripts/docker-flash.sh` | New — similar, exposes USB device. |
| `README.md` | New — quickstart for both local-IDF and Docker paths; board wiring; UART protocol summary. |
| `docs/PRD.md` | New — this document (moved from plan file on repo creation). |
| `docs/uart-protocol.md` | New — full wire format spec. |
| `docs/a1s-pinout.md` | New — A1S 2.2 pin reference (ES8388 I2C/I2S pins, KEY1..6 GPIOs, LEDs, SD card). |
| `docs/build-docker.md` | New — Docker workflow details. |

---

## 8. Implementation Phases

Each phase ends with a concrete verification step run on the actual hardware.

### Phase 1 — Boot + Audio + Demo Tone

**Goal:** Device boots, plays hardcoded Cedar bytecode out of headphone jack.

- Scaffold repo layout (§5.2)
- Cedar IDF component wrapping nkido submodule
- ADF pipeline: minimal I2S sink → ES8388 (no decoders yet)
- Audio task on core 1 calling `cedar::VM::process_block`
- Embedded demo bytecode compiled from `assets/demo.akk` at build time
- `README` with local-IDF quickstart

**Verify:** `idf.py flash monitor`; expect steady tone out of headphones (4.1.2). Measured targets: end-to-end audio latency ≤ 10 ms (ADF input → I2S out at 48 kHz / 128 blk); sustained CPU < 70 % on the audio core for the demo patch; no I2S XRUNs over a 30-minute soak. Record xtensa firmware size (`.bin`) into `docs/size-baseline.md` as the Phase 1 deliverable replacing the host 146 KB figure.

### Phase 2 — UART Bytecode Loader + Hot-Swap

**Goal:** Host can push new bytecode live; device crossfades.

- UART framing + CRC + A/B swap logic on device
- `tools/cedar-push` host tool (Python, uses pyserial)
- Integration with Cedar's existing A/B channel crossfade
- `docs/uart-protocol.md` spec

**Verify:** Compile two different `.akk` patches; push each; confirm audible crossfade (<20 ms) with no clicks. Measured target: UART-push-to-audible-change latency ≤ 200 ms end-to-end (host `cedar-push` invocation → first block of the new program reaches the codec).

### Phase 3 — Hardware Controls (KEY1–KEY6)

**Goal:** Physical buttons drive Akkado `param`/`toggle`/`button`.

- GPIO ISR + debouncer for 6 keys
- Key → param slot mapping (first N declarations in bytecode metadata)
- Bytecode parameter-metadata format (stable across patches)

**Verify:** Patch with `cutoff = param(...)`, `mute = toggle(...)`, `hit = button(...)`; verify KEY1 steps cutoff, KEY2 triggers, KEY3 toggles mute.

### Phase 4 — Sample Decode via ESP-ADF

**Goal:** Akkado can reference sample files decoded by ADF.

- ADF decoder pipeline: file/embedded → WAV/MP3/FLAC decoder → raw PCM in PSRAM
- Cedar sampler hook to reference PSRAM PCM by ID (may require small Cedar API addition — see Impact Assessment open question)
- Embed one demo sample, reference it from a patch

**Verify:** Patch that triggers a sample on KEY2; confirm playback, no glitches, correct pitch at multiple rates.

### Phase 5 — Docker Build Path

**Goal:** `./scripts/docker-build.sh && ./scripts/docker-flash.sh /dev/ttyUSB0` works end-to-end.

- Dockerfile, helper scripts, docs
- CI optional (not required for this PRD)

**Verify:** On a clean machine with only Docker installed, full flow produces a working device.

### Future (out of scope for this PRD)

- SD card as sample source
- WiFi live-coding protocol (web editor targets device directly)
- OTA updates
- MIDI input (USB/serial/BLE)
- ESP32-C3 port with aggressive size trims
- Non-PSRAM builds with reduced arena

---

## 9. Edge Cases

1. **No PSRAM on this specific A1S unit.** Boot detects missing PSRAM, logs warning, refuses to run with default 256 KB arena. User must `menuconfig` → reduce `CEDAR_ARENA_SIZE` to ≤64 KB. Phase 1 exits cleanly with an error message rather than crashing.
2. **UART bytecode larger than `MAX_PROGRAM_SIZE`.** Loader rejects with `NACK[reason=PROGRAM_TOO_LARGE]` before corrupting the inactive channel.
3. **Malformed bytecode (bad CRC or invalid opcodes).** Loader rejects with `NACK[reason=CRC_FAIL]` or `NACK[reason=BAD_BYTECODE]`; active channel keeps running.
4. **Bytecode swap mid-block.** Swap only happens between blocks; no partial writes. Atomic pointer flip is the only synchronization with audio task.
5. **KEY held down at boot.** Debouncer treats initial state as "not pressed" until first release; avoids bogus triggers.
6. **Sample decode failure (Phase 4).** ADF error propagated via log; Cedar sampler ID returns silence rather than UB.
7. **I2S DMA underrun under heavy DSP load.** Audio task priority set above everything except IDF-critical tasks; UART loader runs on core 0 to avoid stealing cycles.
8. **Simultaneous `monitor` output + UART frame.** Monitor output is plain ASCII; frame parser requires the 2-byte magic `0xCE 0xDA` — collisions with log text are astronomically unlikely and non-destructive (CRC rejects).
9. **Runtime-hung patch (valid bytecode but pathological behavior: infinite recursion, extreme CPU load, DMA starvation).** The IDF task watchdog is armed on the audio task. On timeout, the device rolls back to the prior active bytecode (the inactive slot retains the last-known-good program during a pending swap) and logs `watchdog_rollback` over UART. If the already-running active program triggers the watchdog (no rollback target), the device resets cleanly — bootloader comes back up, UART loader is immediately available.

---

## 10. Testing / Verification

**No unit tests on device.** Cedar's existing Catch2 tests in `cedar/tests/` cover VM correctness on the host; that coverage is sufficient and is not duplicated here. Device tests are manual, hardware-in-the-loop, tied to the phase verification steps above.

### Manual test matrix (final acceptance)

| # | Test | Expected |
|---|---|---|
| 1 | `idf.py flash monitor` on fresh checkout | Boots, prints `cedar-esp32 ready`, demo tone audible |
| 2 | `cedar-push patch_a.cbc` | ACK; audio morphs to patch A within 20 ms |
| 3 | `cedar-push patch_b.cbc` (immediately after) | ACK; audio morphs to patch B within 20 ms; no clicks |
| 4 | Hold KEY1 | Cutoff param increments by configured step |
| 5 | Press KEY2 (button) | Trigger pulse audible in patch |
| 6 | Press KEY3 (toggle) | Mute state flips |
| 7 | `cedar-push oversized.cbc` | NACK; device keeps playing prior patch |
| 8 | `cedar-push corrupt.cbc` (bad CRC) | NACK; device keeps playing prior patch |
| 9 | Patch references sample by ID | Sample plays at correct pitch, no glitches |
| 10 | Yank USB mid-playback, replug, `idf.py monitor` | Device recovered to running state; audio resumes |

### Size regression guard

After Phase 1 merges, `scripts/cedar-size-report.sh` output is checked into `cedar-esp32/docs/size-baseline.md`. Future changes that grow firmware by >10 KB need explicit justification in the commit message. Not CI-enforced — a convention, not a gate.

### Docker equivalence check

On a machine with *only* Docker:

```bash
git clone --recurse-submodules git@github.com:<user>/cedar-esp32.git
cd cedar-esp32
./scripts/docker-build.sh
# → build/cedar-esp32.bin SHA256 must match local-IDF build at same commit
```

---

## 11. Open Questions

1. **Zero-copy sample reference for ADF-decoded PCM.** `SampleBank::load_sample(name, const float* data, num_frames, channels, sr)` (see `cedar/include/cedar/vm/sample_bank.hpp:91`) already accepts pre-decoded PCM, but the implementation `memcpy`s into a `std::vector<float>` (`sample_bank.hpp:117`). With a ~2 MB ADF decode budget plus a duplicate Cedar copy, we approach half of typical A1S PSRAM on a single large sample. Phase 4 decides: (a) absorb the duplication (simpler, fits on 8 MB PSRAM boards), or (b) add `SampleBank::register_sample_view(name, std::span<const float>, channels, sr)` that stores a non-owning view into ADF-owned PSRAM. Revisit after Phase 2 when runtime surface is firmer.
2. **Key mapping semantics for overflow params.** If a patch declares 8 `param()`s but the device has 6 keys, are params 7–8 unreachable, or do we pack `param`s and `toggle`s separately? Defer to Phase 3; suggest packing all three kinds into a single ordered list of up to 6.
3. **donny681/esp-adf licensing + drift.** Fork is community-maintained. If it goes stale, Phase 5+ may need to migrate to mainline with custom A1S 2.2 board code. Monitor; no action needed for Phase 1.
4. **C++ exceptions in ESP-IDF.** IDF defaults to `CONFIG_COMPILER_CXX_EXCEPTIONS=n`. Cedar is C++ and uses STL containers (`std::unordered_map`, `std::vector`) in `SampleBank` and `SoundFontRegistry`, which throw on allocation failure. Phase 1 must decide: (a) enable exceptions in `sdkconfig.defaults` and accept the code-size cost (~30–60 KB typical), or (b) add a `-DCEDAR_NO_EXCEPTIONS` build path that uses `std::nothrow` / status returns at the STL boundary. Revisit as a Phase 1 deliverable — measurable once the first xtensa build succeeds.
