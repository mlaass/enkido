> **Status: SHIPPED (external)** — Implemented in sibling repository `~/workspace/godot-nkido-addon`. This PRD is retained here as the original design doc; all further work and documentation lives in that repo.

# PRD: Godot Extension for Nkido (v1 MVP)

**Date:** 2026-03-28

---

## 1. Overview

### 1.1 Problem Statement

Game developers using Godot Engine need ways to create dynamic, reactive audio that responds to game state. Current solutions require external DAW/middleware integration (Wwise, FMOD) with licensing costs, pre-rendered audio variations with limited adaptivity, or custom audio code that's hard to iterate on.

### 1.2 Proposed Solution

Create a GDExtension that embeds Nkido (Akkado + Cedar) directly into Godot 4.6, allowing real-time audio synthesis from Akkado source code with parameter binding from game state.

### 1.3 Goals (v1)

- **Compile and play**: Write Akkado code in the inspector, hear audio output in Godot
- **Parameter binding**: Control `param()`, `button()`, `toggle()`, `dropdown()` from GDScript
- **Hot-swap**: Recompile while playing, hear smooth crossfade to new program
- **Inspector UI**: Multiline text editor, transport buttons, auto-generated parameter controls
- **Global BPM**: Shared tempo via NkidoEngine singleton

### 1.4 Non-Goals (v1)

- Sample loading (`sample()`, SoundFont) — synthesis-only for v1
- `.akk` file loading / resource importer — inline source strings only
- Bottom panel / dedicated code editor — inspector integration only
- Multi-platform builds — Linux x86_64 only
- CI/CD pipeline
- Spatial audio, web export, mobile platforms
- Custom node icons
- Recording / bouncing audio to files

---

## 2. User Experience

### 2.1 Basic Usage

```gdscript
extends Node2D

@onready var synth: NkidoPlayer = $NkidoPlayer

func _ready():
    synth.source = '''
        freq = param("freq", 440, 20, 2000)
        vol = param("volume", 0.8, 0, 1)

        saw(freq) |> lpf(@, 2000) * vol |> out(@, @)
    '''
    synth.compile()
    synth.play()

func _process(delta):
    var health_ratio = player.health / player.max_health
    synth.set_param("freq", lerp(200, 2000, health_ratio))
```

### 2.2 Inspector Integration

When an `NkidoPlayer` node is selected in the scene tree:

```
+-----------------------------------------+
| NkidoPlayer                            |
+-----------------------------------------+
| Source                                  |
| +-------------------------------------+ |
| | saw(220) |> out(@, @)        | |
| |                                     | |
| |                                     | |
| +-------------------------------------+ |
|                                         |
| [Compile] [Play] [Stop]                |
| Status: Compiled successfully           |
|                                         |
| > Parameters (auto-detected)            |
|   freq     [========|--] 440           |
|   volume   [====|------] 0.80          |
|   trigger  [  Button  ]                |
|                                         |
| > Settings                              |
|   BPM: [120    ] (0 = use global)       |
|   Autoplay: [ ]                         |
+-----------------------------------------+
```

### 2.3 Adaptive Music Example

```gdscript
extends Node

@onready var music: NkidoPlayer = $Music

var combat_intensity: float = 0.0

func _ready():
    music.source = '''
        intensity = param("intensity", 0, 0, 1)

        // Base oscillator
        pad = saw(110) |> lpf(@, 200 + intensity * 3000)

        // Lead fades in with intensity
        lead = osc("square", 440) * intensity

        (pad * 0.5 + lead * 0.3) |> out(@, @)
    '''
    music.compile()
    music.play()

func _process(delta):
    var enemy_count = get_tree().get_nodes_in_group("enemies").size()
    var target = clamp(enemy_count / 5.0, 0, 1)
    combat_intensity = lerp(combat_intensity, target, delta * 2)
    music.set_param("intensity", combat_intensity)
```

### 2.4 Sound Effect with Button Trigger

```gdscript
extends CharacterBody2D

@onready var sfx: NkidoPlayer = $JumpSFX

func _ready():
    sfx.source = '''
        pitch = param("pitch", 1, 0.8, 1.2)
        trig = button("play")

        env = ar(trig, 0.01, 0.3)
        sine(440 * pitch) * env |> out(@, @)
    '''
    sfx.compile()
    sfx.play()  # Runs silently until triggered

func jump():
    sfx.set_param("pitch", randf_range(0.9, 1.1))
    sfx.trigger_button("play")
```

---

## 3. Architecture

### 3.1 Repository Structure

```
~/workspace/godot-nkido/                    # Godot project root
    project.godot
    godot-cpp/                              # git submodule (godot-4.6-stable tag)
    nkido/                                 # git submodule
    addons/
        nkido/
            plugin.cfg                      # Editor plugin registration
            nkido.gdextension              # GDExtension manifest
            nkido_plugin.gd                # @tool EditorPlugin
            nkido_inspector.gd             # @tool EditorInspectorPlugin
            native_src/
                SConstruct                  # SCons build file
                src/
                    register_types.cpp
                    register_types.h
                    nkido_engine.cpp
                    nkido_engine.h
                    nkido_player.cpp
                    nkido_player.h
                    nkido_audio_stream.cpp
                    nkido_audio_stream.h
                    nkido_audio_stream_playback.cpp
                    nkido_audio_stream_playback.h
    Main.tscn                               # Demo scene
    Main.gd
```

Submodules live at the project root for clean relative paths from `native_src/`:
- `../../../godot-cpp` (3 levels up: native_src -> nkido -> addons -> root)
- `../../../nkido` (same)

### 3.2 Class Diagram

```
+-------------------------------------------+
|            NkidoEngine                    |
|            (Object singleton)              |
+-------------------------------------------+
| - global_bpm_: float = 120                |
| - sample_rate_: float (from AudioServer)  |
+-------------------------------------------+
| + set_bpm(bpm: float)                     |
| + get_bpm() -> float                      |
| + get_sample_rate() -> float              |
+-------------------------------------------+
                    |
                    | referenced by
                    v
+-------------------------------------------+
|            NkidoPlayer                    |
|            (Node)                          |
+-------------------------------------------+
| - vm_: unique_ptr<cedar::VM>              |
| - source_: String                         |
| - compiled_: bool                         |
| - param_decls_: vector<ParamDecl>         |
| - stream_player_: AudioStreamPlayer*      |
| - audio_stream_: Ref<NkidoAudioStream>   |
+-------------------------------------------+
| + set_source(code: String)                |
| + get_source() -> String                  |
| + compile() -> bool                       |
| + play() / stop() / pause()              |
| + is_playing() -> bool                    |
| + set_param(name, value, slew_ms=20)      |
| + get_param(name) -> float                |
| + trigger_button(name)                    |
| + get_param_decls() -> Array              |
| + get_diagnostics() -> Array              |
+-------------------------------------------+
| Signals:                                  |
| + compilation_finished(success, errors)   |
| + params_changed(params)                  |
| + playback_started()                      |
| + playback_stopped()                      |
+-------------------------------------------+
                    |
                    | creates
                    v
+-------------------------------------------+
|         NkidoAudioStream                  |
|         (AudioStream)                      |
+-------------------------------------------+
| - player_: NkidoPlayer* (non-owning)     |
+-------------------------------------------+
| + _instantiate_playback()                 |
| + _get_length() -> 0.0 (infinite)        |
| + _is_monophonic() -> true                |
+-------------------------------------------+
                    |
                    | creates
                    v
+-------------------------------------------+
|    NkidoAudioStreamPlayback               |
|    (AudioStreamPlayback)                   |
+-------------------------------------------+
| - player_: NkidoPlayer*                  |
| - ring_buffer_: AudioFrame[4096]          |
| - temp_left_/right_: float[128]           |
+-------------------------------------------+
| + _mix(buffer, rate_scale, frames) -> int |
| + _start(from_pos)                        |
| + _stop()                                 |
| + _is_playing() -> bool                   |
+-------------------------------------------+
```

### 3.3 Threading Model

```
+-------------------------------------------+
|            Main Thread                     |
|  (GDScript, Inspector, Editor)            |
+-------------------------------------------+
|  NkidoPlayer::compile()                  |
|    - akkado::compile() (typically < 1ms)  |
|    - Apply state_inits to StatePool       |
|    - vm.load_program() (queues swap)      |
|                                           |
|  NkidoPlayer::set_param("vol", 0.5)     |
|    - vm.set_param() (lock-free atomic)    |
+--------------------+----------------------+
                     | Lock-free (Cedar design)
                     v
+-------------------------------------------+
|            Audio Thread                    |
|  (Godot AudioServer callback)             |
+-------------------------------------------+
|  NkidoAudioStreamPlayback::_mix()        |
|    while ring_buffer needs more data:     |
|      vm.process_block(left, right)  [128] |
|      interleave into ring buffer          |
|    copy frames from ring to output buffer |
+-------------------------------------------+
```

**Thread safety** is guaranteed by Cedar's architecture:
- `load_program()`: triple-buffer swap at block boundary
- `set_param()`: lock-free atomic writes via EnvMap
- `process_block()`: audio-thread-only, reads from current program buffer
- No mutexes needed in the extension code

### 3.4 Audio Pipeline: Block Size Adaptation

Cedar processes fixed 128-sample blocks. Godot's `_mix()` requests arbitrary frame counts (typically 512 or 1024). A ring buffer bridges the mismatch:

```
Cedar VM                Ring Buffer (4096 frames)         Godot _mix()
  process_block() --+
    128 samples     |-->  [write pointer]
  process_block() --+         |
    128 samples     |-->      |                    <-- read N frames
  process_block() --+         v                        into output buffer
    128 samples     |--> [...AudioFrame data...]
```

In `_mix(AudioFrame* buffer, float rate_scale, int32_t frames)`:
1. While ring buffer has fewer than `frames` available: call `vm.process_block()`, interleave 128 L/R samples as AudioFrames into ring
2. Copy `frames` from ring buffer to output
3. Return `frames`

---

## 4. API Reference

### 4.1 NkidoEngine (Singleton)

```gdscript
# Global BPM (affects all players unless overridden)
NkidoEngine.set_bpm(120.0)
var bpm = NkidoEngine.get_bpm()

# Audio sample rate (read-only, from AudioServer)
var rate = NkidoEngine.get_sample_rate()
```

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `bpm` | float | 120.0 | Global BPM for all players |

### 4.2 NkidoPlayer (Node)

#### Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `source` | String | "" | Akkado source code (multiline) |
| `bpm` | float | 0.0 | Per-player BPM override (0 = use global) |
| `crossfade_blocks` | int | 3 | Hot-swap crossfade duration (1-10) |
| `autoplay` | bool | false | Compile and play on `_ready()` |

#### Methods

```gdscript
# Compilation
func compile() -> bool
    # Compiles source, loads into VM, emits compilation_finished
    # Returns true on success

func get_diagnostics() -> Array
    # Returns [{line: int, column: int, message: String}, ...]

# Playback
func play()
func stop()
func pause()
func is_playing() -> bool

# Parameters
func set_param(name: String, value: float, slew_ms: float = 20.0)
func get_param(name: String) -> float
func trigger_button(name: String)
    # Sets param to 1.0 for ~one frame, then releases to 0.0

func get_param_decls() -> Array
    # Returns [{
    #     name: String,
    #     type: String,        # "continuous", "button", "toggle", "select"
    #     default: float,
    #     min: float,
    #     max: float,
    #     options: Array       # For "select" type
    # }, ...]

# Timing
func set_bpm(bpm: float)
func get_bpm() -> float
    # Returns local override, or global if local == 0
```

#### Signals

```gdscript
signal compilation_finished(success: bool, errors: Array)
signal params_changed(params: Array)
signal playback_started()
signal playback_stopped()
```

---

## 5. Implementation Details

### 5.1 GDExtension Entry Point

```cpp
// src/register_types.cpp

#include "register_types.h"
#include "nkido_engine.h"
#include "nkido_player.h"
#include "nkido_audio_stream.h"
#include "nkido_audio_stream_playback.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/classes/engine.hpp>

#include <cedar/cedar.hpp>

using namespace godot;

void initialize_nkido_native_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;

    ClassDB::register_class<NkidoEngine>();
    ClassDB::register_class<NkidoAudioStream>();
    ClassDB::register_class<NkidoAudioStreamPlayback>();
    ClassDB::register_class<NkidoPlayer>();

    // Create singleton (cedar::init called in constructor)
    Engine::get_singleton()->register_singleton(
        "NkidoEngine", memnew(NkidoEngine));
}

void uninitialize_nkido_native_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;

    auto* engine = Object::cast_to<NkidoEngine>(
        Engine::get_singleton()->get_singleton("NkidoEngine"));
    Engine::get_singleton()->unregister_singleton("NkidoEngine");
    memdelete(engine);

    cedar::shutdown();
}

extern "C" {
    GDExtensionBool GDE_EXPORT nkido_native_init(
        GDExtensionInterfaceGetProcAddress p_get_proc_address,
        const GDExtensionClassLibraryPtr p_library,
        GDExtensionInitialization *r_initialization
    ) {
        godot::GDExtensionBinding::InitObject init_obj(
            p_get_proc_address, p_library, r_initialization);

        init_obj.register_initializer(initialize_nkido_native_module);
        init_obj.register_terminator(uninitialize_nkido_native_module);
        init_obj.set_minimum_library_initialization_level(
            MODULE_INITIALIZATION_LEVEL_SCENE);

        return init_obj.init();
    }
}
```

### 5.2 Build System (SConstruct)

Located at `addons/nkido/native_src/SConstruct`. Follows the pattern from `godot_herringbone_wang` with C++20 and nkido source compilation.

```python
# Key configuration:
# - C++20 (nkido requirement)
# - Include paths: godot-cpp, cedar, akkado, src
# - Sources: cedar/*.cpp + akkado/*.cpp + src/*.cpp
# - Output: ../libnkido_native.{arch}.{platform}.{target}.so
```

**Cedar sources** (~7 files from `cedar/src/`):
- `cedar.cpp`, `vm/vm.cpp`, `opcodes/minblep_table.cpp`, `opcodes/minblep_state.cpp`, `io/audio_decoder.cpp`, `io/file_loader.cpp`, `audio/soundfont.cpp`

**Akkado sources** (~24 files from `akkado/src/`)

**Extension sources** (4 files from `src/`)

### 5.3 .gdextension

```ini
[configuration]
entry_symbol = "nkido_native_init"
compatibility_minimum = 4.6

[libraries]
linux.debug.x86_64 = "res://addons/nkido/libnkido_native.x86_64.linux.debug.so"
linux.release.x86_64 = "res://addons/nkido/libnkido_native.x86_64.linux.release.so"
```

### 5.4 NkidoPlayer Core Flow

**Compile:**
```cpp
bool NkidoPlayer::compile() {
    std::string code_str = source_.utf8().get_data();
    auto result = akkado::compile(code_str);

    if (result.success) {
        // Apply state inits BEFORE load_program
        for (const auto& si : result.state_inits) {
            // Apply sequence programs, poly alloc, timeline states
        }

        // Load bytecode (queues hot-swap for next block boundary)
        auto* insts = reinterpret_cast<const cedar::Instruction*>(
            result.bytecode.data());
        size_t count = result.bytecode.size() / sizeof(cedar::Instruction);
        vm_->load_program(std::span{insts, count});

        // Store param declarations
        param_decls_ = std::move(result.param_decls);

        // Initialize params with defaults
        for (const auto& p : param_decls_) {
            vm_->set_param(p.name.c_str(), p.default_value);
        }
    }

    emit_signal("compilation_finished", result.success, diagnostics_array);
    if (result.success) emit_signal("params_changed", get_param_decls());
    return result.success;
}
```

**Play:**
```cpp
void NkidoPlayer::_ready() {
    // Create internal AudioStreamPlayer child
    stream_player_ = memnew(AudioStreamPlayer);
    add_child(stream_player_);

    audio_stream_.instantiate();
    audio_stream_->set_player(this);
    stream_player_->set_stream(audio_stream_);

    // Set sample rate from NkidoEngine
    vm_->set_sample_rate(NkidoEngine::get_sample_rate());

    if (autoplay_ && !source_.is_empty()) {
        if (compile()) play();
    }
}

void NkidoPlayer::play() {
    if (!compiled_) return;
    stream_player_->play();
    emit_signal("playback_started");
}
```

### 5.5 AudioStreamPlayback (_mix)

```cpp
int NkidoAudioStreamPlayback::_mix(
    AudioFrame* p_buffer, float p_rate_scale, int32_t p_frames
) {
    if (!player_ || !player_->is_playing()) {
        for (int i = 0; i < p_frames; i++)
            p_buffer[i] = AudioFrame(0, 0);
        return p_frames;
    }

    auto* vm = player_->get_vm();
    vm->set_bpm(player_->get_bpm());

    // Fill ring buffer as needed
    while (ring_available() < (size_t)p_frames) {
        vm->process_block(temp_left_, temp_right_);
        for (int i = 0; i < 128; i++) {
            ring_buffer_[ring_write_ & (RING_SIZE - 1)] =
                AudioFrame(temp_left_[i], temp_right_[i]);
            ring_write_++;
        }
    }

    // Copy from ring buffer to output
    for (int i = 0; i < p_frames; i++) {
        p_buffer[i] = ring_buffer_[ring_read_ & (RING_SIZE - 1)];
        ring_read_++;
    }

    return p_frames;
}
```

---

## 6. Inspector Plugin

GDScript-based `EditorInspectorPlugin`, registered via `plugin.cfg`.

### plugin.cfg

```ini
[plugin]
name="Nkido Audio Engine"
description="Live-coding audio synthesis for Godot"
author=""
version="0.1.0"
script="nkido_plugin.gd"
```

### nkido_plugin.gd

```gdscript
@tool
extends EditorPlugin

var inspector_plugin: EditorInspectorPlugin

func _enter_tree() -> void:
    inspector_plugin = preload("nkido_inspector.gd").new()
    add_inspector_plugin(inspector_plugin)

func _exit_tree() -> void:
    remove_inspector_plugin(inspector_plugin)
```

### nkido_inspector.gd

The inspector plugin provides:

1. **Source editor**: A `TextEdit` bound to the `source` property with multiline editing
2. **Transport controls**: Compile / Play / Stop buttons in an HBoxContainer
3. **Status label**: Shows "Compiled successfully" or error messages with line numbers
4. **Parameter controls** (auto-generated from `get_param_decls()`):
   - `continuous` -> HSlider with label and value display
   - `button` -> Button (calls `trigger_button()` on press)
   - `toggle` -> CheckButton (calls `set_param(name, 1/0)`)
   - `select` -> OptionButton (calls `set_param(name, idx)`)

Controls are rebuilt when `params_changed` signal fires after recompilation.

---

## 7. Implementation Phases

### Phase 1: Skeleton + Compile Pipeline

**Goal:** Extension loads in Godot. Can compile Akkado source from GDScript.

1. Create `~/workspace/godot-nkido` with `project.godot`
2. Add git submodules: `godot-cpp` (4.6 tag), `nkido`
3. Build `godot-cpp` for Linux (`scons platform=linux target=template_release`)
4. Write `SConstruct` compiling all Cedar + Akkado + extension sources at C++20
5. Implement `register_types.cpp`, `nkido_engine.cpp`, `nkido_player.cpp` (compile only, no audio)
6. Write `.gdextension` and `plugin.cfg`
7. Build and verify: `NkidoEngine.get_sample_rate()` works from GDScript console

**Files:**
| File | Change |
|------|--------|
| `addons/nkido/native_src/SConstruct` | New |
| `addons/nkido/native_src/src/register_types.cpp` | New |
| `addons/nkido/native_src/src/register_types.h` | New |
| `addons/nkido/native_src/src/nkido_engine.cpp` | New |
| `addons/nkido/native_src/src/nkido_engine.h` | New |
| `addons/nkido/native_src/src/nkido_player.cpp` | New (compile only) |
| `addons/nkido/native_src/src/nkido_player.h` | New |
| `addons/nkido/nkido.gdextension` | New |
| `addons/nkido/plugin.cfg` | New |
| `project.godot` | New |

**Verification:** Add NkidoPlayer to scene, set `source = "osc(\"sin\", 440) |> out(@, @)"`, call `compile()`, confirm `compilation_finished` signal fires with `success = true`.

### Phase 2: Audio Playback + Parameters + Hot-Swap

**Goal:** Hear audio output. Control parameters from GDScript. Recompile while playing.

1. Implement `NkidoAudioStream` and `NkidoAudioStreamPlayback` with ring buffer
2. Wire up `NkidoPlayer::play()` / `stop()` / `pause()` via internal AudioStreamPlayer
3. Implement `set_param()` / `get_param()` / `trigger_button()` / `get_param_decls()`
4. Apply `state_inits` from compilation (sequences, poly, timelines)
5. Verify hot-swap: recompile while playing, hear smooth crossfade

**Files:**
| File | Change |
|------|--------|
| `addons/nkido/native_src/src/nkido_audio_stream.cpp` | New |
| `addons/nkido/native_src/src/nkido_audio_stream.h` | New |
| `addons/nkido/native_src/src/nkido_audio_stream_playback.cpp` | New |
| `addons/nkido/native_src/src/nkido_audio_stream_playback.h` | New |
| `addons/nkido/native_src/src/nkido_player.cpp` | Modified (add play/stop/params) |

**Verification:**
- `sine(440) |> out(@, @)` -> hear 440 Hz sine
- `param("freq", 440, 20, 2000) |> sine(@) |> out(@, @)` -> `set_param("freq", 880)` changes pitch
- Change source while playing, call `compile()` -> smooth transition

### Phase 3: Inspector Plugin

**Goal:** Full editor integration with source editing, transport, and parameter controls.

1. Write `nkido_plugin.gd` (EditorPlugin)
2. Write `nkido_inspector.gd` (EditorInspectorPlugin):
   - TextEdit for source property
   - Compile / Play / Stop buttons
   - Status label with compile result
   - Auto-generated parameter controls from `get_param_decls()`
3. Connect to `compilation_finished` and `params_changed` signals

**Files:**
| File | Change |
|------|--------|
| `addons/nkido/nkido_plugin.gd` | New |
| `addons/nkido/nkido_inspector.gd` | New |

**Verification:** Select NkidoPlayer in scene tree. See source editor, transport buttons, and parameter sliders in inspector. Edit source, press Compile, press Play -> hear audio. Adjust slider -> hear parameter change in real-time.

---

## 8. Edge Cases

**Empty source:** `compile()` returns false, emits `compilation_finished(false, [{message: "No source code provided"}])`.

**Compile error:** `compile()` returns false, diagnostics array contains line/column/message for each error. Player state is unchanged (old program keeps playing if already running).

**Play before compile:** `play()` is a no-op if `compiled_` is false.

**Multiple rapid recompiles:** Each `compile()` call queues a new hot-swap. Cedar handles this by replacing the pending program — only the latest compiled program is swapped in.

**NkidoPlayer removed from tree:** `_exit_tree()` stops the internal AudioStreamPlayer, audio stops immediately.

**Large source code:** `akkado::compile()` runs synchronously on the main thread. For typical game audio code (< 100 lines), this is < 1ms. If compilation latency becomes an issue with very large programs, async compilation can be added in a future version.

---

## 9. Testing Strategy

### Manual Testing (v1)

Since the extension runs inside Godot, testing is primarily manual:

1. **Sine wave test**: Source `sine(440) |> out(@, @)` -> compile -> play -> hear 440 Hz
2. **Parameter test**: Add `param("freq", 440, 20, 2000)`, verify `set_param` from GDScript console
3. **Hot-swap test**: Change source while playing, recompile -> no clicks/pops
4. **Error test**: Set source to invalid code, compile -> verify error diagnostics
5. **Inspector test**: Select node -> verify UI controls appear and function
6. **Button trigger test**: Source with `button("hit")` + `ar()` envelope -> trigger from GDScript

### Build Verification

```bash
cd ~/workspace/godot-nkido/addons/nkido/native_src
scons platform=linux target=template_release
# Should produce: ../libnkido_native.x86_64.linux.release.so

cd ~/workspace/godot-nkido
godot46 --editor
# Extension loads without errors, NkidoPlayer appears in node list
```

---

## 10. Open Questions

1. **godot-cpp 4.6 tag**: Does a `godot-4.6-stable` tag exist in godot-cpp, or do we need `4.5-stable`? Check compatibility.
2. **AudioServer availability**: Is `AudioServer::get_singleton()` available at `MODULE_INITIALIZATION_LEVEL_SCENE` during editor startup? May need fallback to 48000 Hz.
3. **Bytecode format**: `CompileResult::bytecode` is `vector<uint8_t>`. Verify this is a flat array of `cedar::Instruction` structs that can be `reinterpret_cast`'d, or whether a different API is needed.

---

## 11. Impact Assessment

| Component | Status | Notes |
|-----------|--------|-------|
| nkido/cedar | **Stays** | Used as-is via submodule, no modifications |
| nkido/akkado | **Stays** | Used as-is via submodule, no modifications |
| godot-cpp | **Stays** | Standard dependency, no modifications |
| `~/workspace/godot-nkido` | **New** | Entire project is new |

---

## 12. Future Work (post-v1)

- **Sample loading**: `NkidoEngine.load_sample()` using Godot's file system, WAV/OGG support
- **`.akk` file resource**: Custom importer so Akkado files appear as resources in the editor
- **Bottom panel editor**: Larger code editing area with syntax highlighting
- **Multi-platform**: Windows + macOS builds, CI/CD pipeline
- **Custom node icon**: SVG icon for NkidoPlayer in the scene tree
- **Spatial audio**: 3D positioning via AudioStreamPlayer3D
- **Volume/bus properties**: Forward to internal AudioStreamPlayer
- **SoundFont support**: Load .sf2 files for instrument playback
