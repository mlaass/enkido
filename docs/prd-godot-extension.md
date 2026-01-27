# PRD: Godot Extension for Enkido

**Status:** Draft
**Author:** Claude
**Date:** 2026-01-27

## 1. Overview

### 1.1 Problem Statement

Game developers using Godot Engine need ways to create dynamic, reactive audio that responds to game state. Current solutions require:
- External DAW/middleware integration (Wwise, FMOD) with licensing costs
- Pre-rendered audio variations with limited adaptivity
- Custom audio code that's hard to iterate on

### 1.2 Proposed Solution

Create a GDExtension that embeds Enkido (Akkado + Cedar) directly into Godot, allowing:
- Live-coding audio synthesis within the game engine
- Real-time parameter binding from game state to audio
- Zero-latency adaptive music without middleware

### 1.3 Goals

- **Native integration:** Feels like a first-class Godot audio system
- **Real-time:** No audio dropouts, sample-accurate timing
- **Live-coding:** Edit code while game runs, hear changes immediately
- **Parameter binding:** Connect game variables to audio parameters declaratively
- **Portable:** Works on Windows, Linux, macOS, and potentially web exports

### 1.4 Non-Goals (v1)

- Mobile platforms (iOS, Android) - future consideration
- Visual patching interface - text-based for v1
- Recording/bouncing audio to files
- VST/AU plugin format

---

## 2. User Experience

### 2.1 Basic Usage

```gdscript
extends Node2D

@onready var music: EnkidoPlayer = $EnkidoPlayer

func _ready():
    # Load from file
    music.source_file = "res://audio/theme.akk"

    # Or inline source
    music.source = '''
        bpm = 120
        vol = param("volume", 0.8)
        cutoff = param("filter", 2000, 100, 8000)

        bass = osc("saw", 55) |> lpf(%, cutoff) * vol
        bass |> out(%, %)
    '''

    music.compile()
    music.play()

func _process(delta):
    # Bind game state to audio parameters
    var health_ratio = player.health / player.max_health
    music.set_param("filter", lerp(500, 8000, health_ratio))

    # Trigger one-shot sounds
    if Input.is_action_just_pressed("jump"):
        music.trigger_button("jump_sfx")
```

### 2.2 Inspector Integration

When an `EnkidoPlayer` node is selected:

```
┌─────────────────────────────────────────┐
│ EnkidoPlayer                            │
├─────────────────────────────────────────┤
│ Source                                  │
│ ┌─────────────────────────────────────┐ │
│ │ osc("saw", 220) |> out(%, %)        │ │
│ └─────────────────────────────────────┘ │
│                                         │
│ Source File: [                      ] ◉ │
│                                         │
│ ▼ Transport                             │
│   [▶ Play] [■ Stop] BPM: [120    ]     │
│                                         │
│ ▼ Parameters (auto-detected)            │
│   volume    [========|--] 0.80          │
│   filter    [====|------] 2000 Hz       │
│   jump_sfx  [  Trigger  ]               │
│                                         │
│ ▼ Compilation                           │
│   Status: ✓ Compiled successfully       │
│   [Recompile]                           │
│                                         │
│ ▼ Advanced                              │
│   Crossfade Blocks: [3        ]         │
│   Preview in Editor: [✓]                │
└─────────────────────────────────────────┘
```

### 2.3 Adaptive Music Example

```gdscript
# Dynamic music system that responds to combat intensity

extends Node

@onready var music: EnkidoPlayer = $Music

var combat_intensity: float = 0.0

func _ready():
    music.source = '''
        // Parameters from game state
        intensity = param("intensity", 0, 0, 1)
        tension = param("tension", 0, 0, 1)

        // Base drum pattern - always playing
        kick = sample("kick") * seq("x...x...x...x...")
        hat = sample("hat") * seq("..x...x...x...x.")

        // Synth layers - crossfade based on intensity
        pad = osc("saw", 110) |> lpf(%, 200 + intensity * 2000)
        lead = osc("square", 440) |> delay(%, 0.3) * intensity

        // Mix based on intensity
        drums = (kick + hat * 0.5) * 0.8
        synths = pad * 0.3 + lead * 0.5

        (drums + synths * intensity) |> out(%, %)
    '''
    music.compile()
    music.play()

func _process(delta):
    # Update intensity based on nearby enemies
    var enemy_count = get_tree().get_nodes_in_group("enemies").size()
    var target = clamp(enemy_count / 5.0, 0, 1)
    combat_intensity = lerp(combat_intensity, target, delta * 2)

    music.set_param("intensity", combat_intensity)

func on_boss_appear():
    music.set_param("tension", 1.0, 2000)  # 2 second transition
```

### 2.4 Sound Effects

```gdscript
# One-shot sound effects with parameter variation

extends CharacterBody2D

@onready var sfx: EnkidoPlayer = $JumpSFX

func _ready():
    sfx.source = '''
        pitch = param("pitch", 1, 0.8, 1.2)
        trig = button("play")

        env = ar(trig, 0.01, 0.3)
        osc("sine", 440 * pitch) * env |> out(%, %)
    '''
    sfx.compile()
    sfx.play()  # Runs silently until triggered

func jump():
    # Randomize pitch for variation
    sfx.set_param("pitch", randf_range(0.9, 1.1))
    sfx.trigger_button("play")
```

---

## 3. Architecture

### 3.1 Repository Structure

```
enkido-godot/
├── .github/
│   └── workflows/
│       └── build.yml           # CI for Windows/Linux/macOS
├── SConstruct                  # Godot build system
├── enkido/                     # Git submodule → enkido repo
├── godot-cpp/                  # Git submodule → godot-cpp
├── src/
│   ├── register_types.cpp      # GDExtension entry point
│   ├── register_types.hpp
│   ├── enkido_engine.hpp       # Singleton for global state
│   ├── enkido_engine.cpp
│   ├── enkido_player.hpp       # Per-instance player node
│   ├── enkido_player.cpp
│   ├── enkido_audio_stream.hpp # Custom AudioStream
│   ├── enkido_audio_stream.cpp
│   ├── enkido_audio_stream_playback.hpp
│   ├── enkido_audio_stream_playback.cpp
│   └── enkido_resource_loader.hpp  # .akk file importer
├── addons/
│   └── enkido/
│       ├── plugin.cfg          # Editor plugin metadata
│       ├── enkido_inspector.gd # Custom inspector for params
│       └── icons/              # Node icons
├── demo/
│   ├── project.godot
│   ├── Main.tscn
│   ├── Main.gd
│   └── audio/
│       └── theme.akk
├── bin/                        # Built libraries (gitignored)
├── README.md
└── LICENSE
```

### 3.2 Class Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                         EnkidoEngine                                │
│                         (Singleton)                                 │
├─────────────────────────────────────────────────────────────────────┤
│ - sample_bank_: SampleBank                                          │
│ - global_bpm_: float                                                │
│ - sample_rate_: float                                               │
├─────────────────────────────────────────────────────────────────────┤
│ + set_bpm(bpm: float)                                               │
│ + get_bpm() -> float                                                │
│ + load_sample(name: String, path: String) -> int                    │
│ + get_sample_id(name: String) -> int                                │
│ + clear_samples()                                                   │
└─────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ references
                                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         EnkidoPlayer                                │
│                         (Node)                                      │
├─────────────────────────────────────────────────────────────────────┤
│ - vm_: std::unique_ptr<Cedar::VM>                                   │
│ - env_map_: Cedar::EnvMap                                           │
│ - source_: String                                                   │
│ - source_file_: String                                              │
│ - playing_: bool                                                    │
│ - compiled_: bool                                                   │
│ - param_decls_: std::vector<ParamDecl>                              │
├─────────────────────────────────────────────────────────────────────┤
│ + set_source(code: String)                                          │
│ + get_source() -> String                                            │
│ + set_source_file(path: String)                                     │
│ + get_source_file() -> String                                       │
│ + compile() -> bool                                                 │
│ + play()                                                            │
│ + stop()                                                            │
│ + pause()                                                           │
│ + is_playing() -> bool                                              │
│ + set_param(name: String, value: float, slew_ms: float = 20)        │
│ + get_param(name: String) -> float                                  │
│ + trigger_button(name: String)                                      │
│ + get_params() -> Array[Dictionary]                                 │
│ + set_bpm(bpm: float)  // Override global                           │
│ + get_bpm() -> float                                                │
├─────────────────────────────────────────────────────────────────────┤
│ Signals:                                                            │
│ + compilation_finished(success: bool, errors: Array)                │
│ + params_changed(params: Array)                                     │
│ + playback_started()                                                │
│ + playback_stopped()                                                │
└─────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ creates
                                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      EnkidoAudioStream                              │
│                      (AudioStream)                                  │
├─────────────────────────────────────────────────────────────────────┤
│ - player_: EnkidoPlayer*                                            │
├─────────────────────────────────────────────────────────────────────┤
│ + instantiate_playback() -> AudioStreamPlayback                     │
│ + get_length() -> float  // Returns 0 (infinite)                    │
└─────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ creates
                                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│                  EnkidoAudioStreamPlayback                          │
│                  (AudioStreamPlayback)                              │
├─────────────────────────────────────────────────────────────────────┤
│ - player_: EnkidoPlayer*                                            │
│ - output_buffer_: std::vector<AudioFrame>                           │
├─────────────────────────────────────────────────────────────────────┤
│ + _mix(buffer: AudioFrame*, rate: float, frames: int) -> int        │
│ + _start(from: float)                                               │
│ + _stop()                                                           │
│ + _is_playing() -> bool                                             │
└─────────────────────────────────────────────────────────────────────┘
```

### 3.3 Threading Model

```
┌─────────────────────────────────────────────────────────────────────┐
│                          Main Thread                                │
│  (GDScript, Inspector, Editor)                                      │
├─────────────────────────────────────────────────────────────────────┤
│  EnkidoPlayer::compile()                                            │
│    - Parse Akkado source                                            │
│    - Generate bytecode                                              │
│    - Extract param declarations                                     │
│    - Load bytecode into VM (thread-safe swap)                       │
│                                                                     │
│  EnkidoPlayer::set_param("volume", 0.5)                             │
│    - env_map_.set_param("volume", 0.5, 20.0f)                       │
│    - Lock-free atomic write                                         │
└───────────────────────────────┬─────────────────────────────────────┘
                                │ Lock-free EnvMap
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         Audio Thread                                │
│  (Godot AudioServer callback)                                       │
├─────────────────────────────────────────────────────────────────────┤
│  EnkidoAudioStreamPlayback::_mix()                                  │
│    while (frames_remaining > 0) {                                   │
│        vm->process_block();           // Cedar VM execution         │
│        env_map_.update_interpolation_block();                       │
│        copy_output_to_buffer();                                     │
│        frames_remaining -= BLOCK_SIZE;                              │
│    }                                                                │
│                                                                     │
│  ENV_GET opcode reads from EnvMap                                   │
│    - Lock-free atomic read                                          │
│    - Per-sample interpolation for smooth transitions                │
└─────────────────────────────────────────────────────────────────────┘
```

### 3.4 Memory Layout

```
EnkidoPlayer Instance
├── Cedar::VM
│   ├── BufferPool (pre-allocated signal buffers)
│   │   └── 4096 × 128 floats = 2MB
│   ├── StatePool (DSP state storage)
│   │   └── 4096 states × ~256 bytes = 1MB
│   ├── AudioArena (delays, reverbs)
│   │   └── 128MB max (configurable)
│   └── Bytecode (instructions)
│       └── ~64KB typical
├── Cedar::EnvMap
│   └── 4096 params × 24 bytes = ~100KB
└── ParamDecls
    └── ~1KB typical
```

---

## 4. API Reference

### 4.1 EnkidoEngine (Autoload Singleton)

```gdscript
# Access via EnkidoEngine singleton (registered as autoload)

# Set global BPM (affects all players unless overridden)
EnkidoEngine.set_bpm(120.0)
var bpm = EnkidoEngine.get_bpm()

# Load samples globally (shared across all players)
var sample_id = EnkidoEngine.load_sample("kick", "res://samples/kick.wav")

# Check if sample is loaded
var id = EnkidoEngine.get_sample_id("kick")  # Returns -1 if not found

# Clear all samples (e.g., on scene change)
EnkidoEngine.clear_samples()

# Get/set sample rate (usually matches AudioServer)
var rate = EnkidoEngine.get_sample_rate()
```

### 4.2 EnkidoPlayer (Node)

#### Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `source` | String | "" | Akkado source code |
| `source_file` | String | "" | Path to .akk file |
| `playing` | bool | false | Read-only playback state |
| `compiled` | bool | false | Read-only compilation state |
| `bpm` | float | 0.0 | BPM override (0 = use global) |
| `crossfade_blocks` | int | 3 | Hot-swap crossfade duration |
| `autoplay` | bool | false | Start playing on _ready() |

#### Methods

```gdscript
# Compilation
func compile() -> bool:
    # Compiles source/source_file and loads into VM
    # Returns true on success
    # Emits compilation_finished signal

func get_compile_errors() -> Array:
    # Returns array of error dictionaries:
    # [{line: int, column: int, message: String}, ...]

# Playback
func play():
    # Start audio output
    # Does nothing if not compiled

func stop():
    # Stop audio output

func pause():
    # Pause (keeps state, resumes from same point)

func is_playing() -> bool:
    # Returns current playback state

# Parameters
func set_param(name: String, value: float, slew_ms: float = 20.0):
    # Set parameter with optional slew time
    # slew_ms = 0 for immediate change

func get_param(name: String) -> float:
    # Get current parameter value

func trigger_button(name: String):
    # Set parameter to 1.0 for one block, then 0.0
    # Used for button() type params

func get_params() -> Array:
    # Returns array of parameter declarations:
    # [{
    #     name: String,
    #     type: String,  # "continuous", "button", "toggle", "select"
    #     default: float,
    #     min: float,
    #     max: float,
    #     options: Array  # For "select" type only
    # }, ...]

# Timing
func set_bpm(bpm: float):
    # Override global BPM for this player

func get_bpm() -> float:
    # Get effective BPM (local override or global)

func get_beat_position() -> float:
    # Get current beat position (for sync)

func get_cycle_position() -> float:
    # Get current cycle position (0-1)
```

#### Signals

```gdscript
signal compilation_finished(success: bool, errors: Array)
signal params_changed(params: Array)
signal playback_started()
signal playback_stopped()
```

### 4.3 .akk Resource

```gdscript
# Load .akk file as resource
var music_script = load("res://audio/theme.akk")

# Use with EnkidoPlayer
player.source_file = "res://audio/theme.akk"
# OR
player.source = music_script.get_source()
```

---

## 5. Implementation Details

### 5.1 GDExtension Entry Point

```cpp
// src/register_types.cpp

#include "register_types.hpp"
#include "enkido_engine.hpp"
#include "enkido_player.hpp"
#include "enkido_audio_stream.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

void initialize_enkido_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    // Register classes
    ClassDB::register_class<EnkidoEngine>();
    ClassDB::register_class<EnkidoPlayer>();
    ClassDB::register_class<EnkidoAudioStream>();
    ClassDB::register_class<EnkidoAudioStreamPlayback>();

    // Create singleton
    Engine::get_singleton()->register_singleton(
        "EnkidoEngine",
        memnew(EnkidoEngine)
    );
}

void uninitialize_enkido_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    // Clean up singleton
    Engine::get_singleton()->unregister_singleton("EnkidoEngine");
}

extern "C" {
    GDExtensionBool GDE_EXPORT enkido_library_init(
        GDExtensionInterfaceGetProcAddress p_get_proc_address,
        const GDExtensionClassLibraryPtr p_library,
        GDExtensionInitialization *r_initialization
    ) {
        godot::GDExtensionBinding::InitObject init_obj(
            p_get_proc_address, p_library, r_initialization
        );

        init_obj.register_initializer(initialize_enkido_module);
        init_obj.register_terminator(uninitialize_enkido_module);
        init_obj.set_minimum_library_initialization_level(
            MODULE_INITIALIZATION_LEVEL_SCENE
        );

        return init_obj.init();
    }
}
```

### 5.2 EnkidoEngine Singleton

```cpp
// src/enkido_engine.hpp

#pragma once

#include <godot_cpp/classes/object.hpp>
#include <cedar/vm/sample_bank.hpp>
#include <memory>

namespace godot {

class EnkidoEngine : public Object {
    GDCLASS(EnkidoEngine, Object)

private:
    std::unique_ptr<cedar::SampleBank> sample_bank_;
    float global_bpm_ = 120.0f;
    float sample_rate_ = 48000.0f;

protected:
    static void _bind_methods();

public:
    EnkidoEngine();
    ~EnkidoEngine() override;

    // BPM
    void set_bpm(float bpm);
    float get_bpm() const;

    // Sample rate (usually auto-detected from AudioServer)
    void set_sample_rate(float rate);
    float get_sample_rate() const;

    // Sample management
    int load_sample(const String& name, const String& path);
    int get_sample_id(const String& name) const;
    void clear_samples();

    // Internal access for EnkidoPlayer
    cedar::SampleBank* get_sample_bank() const { return sample_bank_.get(); }
};

} // namespace godot
```

```cpp
// src/enkido_engine.cpp

#include "enkido_engine.hpp"
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/file_access.hpp>

namespace godot {

void EnkidoEngine::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_bpm", "bpm"), &EnkidoEngine::set_bpm);
    ClassDB::bind_method(D_METHOD("get_bpm"), &EnkidoEngine::get_bpm);

    ClassDB::bind_method(D_METHOD("set_sample_rate", "rate"),
                         &EnkidoEngine::set_sample_rate);
    ClassDB::bind_method(D_METHOD("get_sample_rate"),
                         &EnkidoEngine::get_sample_rate);

    ClassDB::bind_method(D_METHOD("load_sample", "name", "path"),
                         &EnkidoEngine::load_sample);
    ClassDB::bind_method(D_METHOD("get_sample_id", "name"),
                         &EnkidoEngine::get_sample_id);
    ClassDB::bind_method(D_METHOD("clear_samples"),
                         &EnkidoEngine::clear_samples);

    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bpm"),
                 "set_bpm", "get_bpm");
}

EnkidoEngine::EnkidoEngine() {
    sample_bank_ = std::make_unique<cedar::SampleBank>();

    // Get sample rate from AudioServer
    auto* audio_server = AudioServer::get_singleton();
    if (audio_server) {
        sample_rate_ = static_cast<float>(audio_server->get_mix_rate());
    }
}

EnkidoEngine::~EnkidoEngine() = default;

void EnkidoEngine::set_bpm(float bpm) {
    global_bpm_ = std::max(1.0f, std::min(999.0f, bpm));
}

float EnkidoEngine::get_bpm() const {
    return global_bpm_;
}

int EnkidoEngine::load_sample(const String& name, const String& path) {
    // Load audio file using Godot's file system
    auto file = FileAccess::open(path, FileAccess::READ);
    if (!file.is_valid()) {
        ERR_PRINT("EnkidoEngine: Failed to open sample file: " + path);
        return -1;
    }

    // Read WAV data
    PackedByteArray data = file->get_buffer(file->get_length());

    // Load into sample bank
    std::string name_str = name.utf8().get_data();
    return sample_bank_->load_wav(
        name_str.c_str(),
        reinterpret_cast<const uint8_t*>(data.ptr()),
        data.size()
    );
}

int EnkidoEngine::get_sample_id(const String& name) const {
    std::string name_str = name.utf8().get_data();
    return sample_bank_->get_id(name_str.c_str());
}

void EnkidoEngine::clear_samples() {
    sample_bank_->clear();
}

} // namespace godot
```

### 5.3 EnkidoPlayer Node

```cpp
// src/enkido_player.hpp

#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <cedar/vm/vm.hpp>
#include <cedar/vm/env_map.hpp>
#include <akkado/akkado.hpp>
#include <memory>
#include <atomic>

namespace godot {

class EnkidoAudioStream;

class EnkidoPlayer : public Node {
    GDCLASS(EnkidoPlayer, Node)

private:
    // Audio components
    std::unique_ptr<cedar::VM> vm_;
    cedar::EnvMap env_map_;
    Ref<EnkidoAudioStream> audio_stream_;
    AudioStreamPlayer* stream_player_ = nullptr;

    // Source
    String source_;
    String source_file_;

    // State
    std::atomic<bool> playing_{false};
    bool compiled_ = false;
    float local_bpm_ = 0.0f;  // 0 = use global
    int crossfade_blocks_ = 3;
    bool autoplay_ = false;

    // Parameter metadata (from compilation)
    std::vector<akkado::ParamDecl> param_decls_;

    // Button trigger queue (main thread → audio thread)
    struct ButtonTrigger {
        uint32_t name_hash;
        std::atomic<bool> active{false};
    };
    std::vector<ButtonTrigger> button_triggers_;

protected:
    static void _bind_methods();

public:
    EnkidoPlayer();
    ~EnkidoPlayer() override;

    void _ready() override;
    void _process(double delta) override;

    // Source
    void set_source(const String& code);
    String get_source() const;
    void set_source_file(const String& path);
    String get_source_file() const;

    // Compilation
    bool compile();
    Array get_compile_errors() const;

    // Playback
    void play();
    void stop();
    void pause();
    bool is_playing() const;

    // Parameters
    void set_param(const String& name, float value, float slew_ms = 20.0f);
    float get_param(const String& name) const;
    void trigger_button(const String& name);
    Array get_params() const;

    // Timing
    void set_bpm(float bpm);
    float get_bpm() const;
    float get_beat_position() const;
    float get_cycle_position() const;

    // Internal (for audio stream)
    cedar::VM* get_vm() { return vm_.get(); }
    cedar::EnvMap* get_env_map() { return &env_map_; }
    void process_button_triggers();

    // Properties
    void set_crossfade_blocks(int blocks);
    int get_crossfade_blocks() const;
    void set_autoplay(bool autoplay);
    bool get_autoplay() const;
};

} // namespace godot
```

```cpp
// src/enkido_player.cpp

#include "enkido_player.hpp"
#include "enkido_engine.hpp"
#include "enkido_audio_stream.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>

namespace godot {

void EnkidoPlayer::_bind_methods() {
    // Source properties
    ClassDB::bind_method(D_METHOD("set_source", "code"),
                         &EnkidoPlayer::set_source);
    ClassDB::bind_method(D_METHOD("get_source"),
                         &EnkidoPlayer::get_source);
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "source",
                              PROPERTY_HINT_MULTILINE_TEXT),
                 "set_source", "get_source");

    ClassDB::bind_method(D_METHOD("set_source_file", "path"),
                         &EnkidoPlayer::set_source_file);
    ClassDB::bind_method(D_METHOD("get_source_file"),
                         &EnkidoPlayer::get_source_file);
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "source_file",
                              PROPERTY_HINT_FILE, "*.akk"),
                 "set_source_file", "get_source_file");

    // Compilation
    ClassDB::bind_method(D_METHOD("compile"), &EnkidoPlayer::compile);
    ClassDB::bind_method(D_METHOD("get_compile_errors"),
                         &EnkidoPlayer::get_compile_errors);

    // Playback
    ClassDB::bind_method(D_METHOD("play"), &EnkidoPlayer::play);
    ClassDB::bind_method(D_METHOD("stop"), &EnkidoPlayer::stop);
    ClassDB::bind_method(D_METHOD("pause"), &EnkidoPlayer::pause);
    ClassDB::bind_method(D_METHOD("is_playing"), &EnkidoPlayer::is_playing);

    // Parameters
    ClassDB::bind_method(D_METHOD("set_param", "name", "value", "slew_ms"),
                         &EnkidoPlayer::set_param,
                         DEFVAL(20.0f));
    ClassDB::bind_method(D_METHOD("get_param", "name"),
                         &EnkidoPlayer::get_param);
    ClassDB::bind_method(D_METHOD("trigger_button", "name"),
                         &EnkidoPlayer::trigger_button);
    ClassDB::bind_method(D_METHOD("get_params"), &EnkidoPlayer::get_params);

    // Timing
    ClassDB::bind_method(D_METHOD("set_bpm", "bpm"), &EnkidoPlayer::set_bpm);
    ClassDB::bind_method(D_METHOD("get_bpm"), &EnkidoPlayer::get_bpm);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bpm"), "set_bpm", "get_bpm");

    ClassDB::bind_method(D_METHOD("get_beat_position"),
                         &EnkidoPlayer::get_beat_position);
    ClassDB::bind_method(D_METHOD("get_cycle_position"),
                         &EnkidoPlayer::get_cycle_position);

    // Advanced properties
    ClassDB::bind_method(D_METHOD("set_crossfade_blocks", "blocks"),
                         &EnkidoPlayer::set_crossfade_blocks);
    ClassDB::bind_method(D_METHOD("get_crossfade_blocks"),
                         &EnkidoPlayer::get_crossfade_blocks);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "crossfade_blocks",
                              PROPERTY_HINT_RANGE, "1,10"),
                 "set_crossfade_blocks", "get_crossfade_blocks");

    ClassDB::bind_method(D_METHOD("set_autoplay", "autoplay"),
                         &EnkidoPlayer::set_autoplay);
    ClassDB::bind_method(D_METHOD("get_autoplay"),
                         &EnkidoPlayer::get_autoplay);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "autoplay"),
                 "set_autoplay", "get_autoplay");

    // Signals
    ADD_SIGNAL(MethodInfo("compilation_finished",
                          PropertyInfo(Variant::BOOL, "success"),
                          PropertyInfo(Variant::ARRAY, "errors")));
    ADD_SIGNAL(MethodInfo("params_changed",
                          PropertyInfo(Variant::ARRAY, "params")));
    ADD_SIGNAL(MethodInfo("playback_started"));
    ADD_SIGNAL(MethodInfo("playback_stopped"));
}

EnkidoPlayer::EnkidoPlayer() {
    vm_ = std::make_unique<cedar::VM>();
}

EnkidoPlayer::~EnkidoPlayer() = default;

void EnkidoPlayer::_ready() {
    // Create internal AudioStreamPlayer
    stream_player_ = memnew(AudioStreamPlayer);
    add_child(stream_player_);
    stream_player_->set_name("_EnkidoStreamPlayer");

    // Create audio stream
    audio_stream_.instantiate();
    audio_stream_->set_player(this);
    stream_player_->set_stream(audio_stream_);

    // Initialize VM with sample rate
    auto* engine = Object::cast_to<EnkidoEngine>(
        Engine::get_singleton()->get_singleton("EnkidoEngine")
    );
    if (engine) {
        vm_->set_sample_rate(engine->get_sample_rate());
    }

    // Autoplay if configured
    if (autoplay_ && !source_.is_empty()) {
        if (compile()) {
            play();
        }
    }
}

void EnkidoPlayer::_process(double delta) {
    // Process button triggers
    process_button_triggers();
}

bool EnkidoPlayer::compile() {
    // Get source code
    String code = source_;
    if (!source_file_.is_empty()) {
        auto file = FileAccess::open(source_file_, FileAccess::READ);
        if (file.is_valid()) {
            code = file->get_as_text();
        } else {
            Array errors;
            Dictionary error;
            error["line"] = 0;
            error["column"] = 0;
            error["message"] = "Failed to open file: " + source_file_;
            errors.push_back(error);
            emit_signal("compilation_finished", false, errors);
            return false;
        }
    }

    if (code.is_empty()) {
        Array errors;
        Dictionary error;
        error["line"] = 0;
        error["column"] = 0;
        error["message"] = "No source code provided";
        errors.push_back(error);
        emit_signal("compilation_finished", false, errors);
        return false;
    }

    // Compile
    std::string code_str = code.utf8().get_data();
    auto result = akkado::compile(code_str);

    if (!result.success) {
        Array errors;
        for (const auto& diag : result.diagnostics) {
            if (diag.severity == akkado::DiagnosticSeverity::Error) {
                Dictionary error;
                error["line"] = static_cast<int>(diag.location.line);
                error["column"] = static_cast<int>(diag.location.column);
                error["message"] = String(diag.message.c_str());
                errors.push_back(error);
            }
        }
        emit_signal("compilation_finished", false, errors);
        return false;
    }

    // Load bytecode into VM
    vm_->load_program(result.bytecode.data(), result.bytecode.size());

    // Apply state initializations
    for (const auto& init : result.state_inits) {
        vm_->apply_state_init(init);
    }

    // Store parameter declarations
    param_decls_ = std::move(result.param_decls);

    // Initialize parameters with defaults
    for (const auto& param : param_decls_) {
        env_map_.set_param(param.name.c_str(), param.default_value, 0.0f);
    }

    // Set up button triggers
    button_triggers_.clear();
    for (const auto& param : param_decls_) {
        if (param.type == akkado::ParamType::Button) {
            ButtonTrigger trigger;
            trigger.name_hash = param.name_hash;
            button_triggers_.push_back(std::move(trigger));
        }
    }

    compiled_ = true;

    // Emit signals
    emit_signal("compilation_finished", true, Array());
    emit_signal("params_changed", get_params());

    return true;
}

void EnkidoPlayer::play() {
    if (!compiled_) return;

    playing_.store(true);
    stream_player_->play();
    emit_signal("playback_started");
}

void EnkidoPlayer::stop() {
    playing_.store(false);
    stream_player_->stop();
    vm_->reset();
    emit_signal("playback_stopped");
}

void EnkidoPlayer::pause() {
    playing_.store(false);
    stream_player_->stop();
}

bool EnkidoPlayer::is_playing() const {
    return playing_.load();
}

void EnkidoPlayer::set_param(const String& name, float value, float slew_ms) {
    std::string name_str = name.utf8().get_data();
    env_map_.set_param(name_str.c_str(), value, slew_ms);
}

float EnkidoPlayer::get_param(const String& name) const {
    std::string name_str = name.utf8().get_data();
    uint32_t hash = cedar::fnv1a_hash(name_str.c_str());
    return env_map_.get_target(hash);
}

void EnkidoPlayer::trigger_button(const String& name) {
    std::string name_str = name.utf8().get_data();
    uint32_t hash = cedar::fnv1a_hash(name_str.c_str());

    for (auto& trigger : button_triggers_) {
        if (trigger.name_hash == hash) {
            trigger.active.store(true);
            env_map_.set_param(name_str.c_str(), 1.0f, 0.0f);
            break;
        }
    }
}

void EnkidoPlayer::process_button_triggers() {
    // Release buttons that were triggered
    for (auto& trigger : button_triggers_) {
        if (trigger.active.load()) {
            trigger.active.store(false);

            // Find name and release
            for (const auto& param : param_decls_) {
                if (param.name_hash == trigger.name_hash) {
                    env_map_.set_param(param.name.c_str(), 0.0f, 0.0f);
                    break;
                }
            }
        }
    }
}

Array EnkidoPlayer::get_params() const {
    Array result;

    for (const auto& param : param_decls_) {
        Dictionary dict;
        dict["name"] = String(param.name.c_str());

        switch (param.type) {
            case akkado::ParamType::Continuous:
                dict["type"] = "continuous";
                break;
            case akkado::ParamType::Button:
                dict["type"] = "button";
                break;
            case akkado::ParamType::Toggle:
                dict["type"] = "toggle";
                break;
            case akkado::ParamType::Select:
                dict["type"] = "select";
                break;
        }

        dict["default"] = param.default_value;
        dict["min"] = param.min_value;
        dict["max"] = param.max_value;

        if (!param.options.empty()) {
            Array options;
            for (const auto& opt : param.options) {
                options.push_back(String(opt.c_str()));
            }
            dict["options"] = options;
        }

        result.push_back(dict);
    }

    return result;
}

float EnkidoPlayer::get_bpm() const {
    if (local_bpm_ > 0.0f) {
        return local_bpm_;
    }

    auto* engine = Object::cast_to<EnkidoEngine>(
        Engine::get_singleton()->get_singleton("EnkidoEngine")
    );
    return engine ? engine->get_bpm() : 120.0f;
}

} // namespace godot
```

### 5.4 Custom AudioStream

```cpp
// src/enkido_audio_stream_playback.cpp

#include "enkido_audio_stream_playback.hpp"
#include "enkido_player.hpp"
#include "enkido_engine.hpp"

#include <godot_cpp/classes/engine.hpp>

namespace godot {

int EnkidoAudioStreamPlayback::_mix(
    AudioFrame* p_buffer,
    float p_rate_scale,
    int p_frames
) {
    if (!player_ || !player_->is_playing()) {
        // Fill with silence
        for (int i = 0; i < p_frames; i++) {
            p_buffer[i] = AudioFrame(0, 0);
        }
        return p_frames;
    }

    auto* vm = player_->get_vm();
    auto* env_map = player_->get_env_map();

    // Get BPM from engine or player
    auto* engine = Object::cast_to<EnkidoEngine>(
        Engine::get_singleton()->get_singleton("EnkidoEngine")
    );
    float bpm = player_->get_bpm();
    vm->set_bpm(bpm);

    int frames_processed = 0;
    constexpr int BLOCK_SIZE = cedar::BLOCK_SIZE;  // 128

    while (frames_processed < p_frames) {
        // Process one block
        vm->process_block();
        env_map->update_interpolation_block();

        // Copy output
        const float* left = vm->get_output_left();
        const float* right = vm->get_output_right();

        int frames_to_copy = std::min(BLOCK_SIZE, p_frames - frames_processed);
        for (int i = 0; i < frames_to_copy; i++) {
            p_buffer[frames_processed + i] = AudioFrame(left[i], right[i]);
        }

        frames_processed += frames_to_copy;
    }

    return p_frames;
}

} // namespace godot
```

### 5.5 Build System

```python
# SConstruct

#!/usr/bin/env python
import os
import sys

# Load godot-cpp build environment
env = SConscript("godot-cpp/SConstruct")

# Add enkido include paths
env.Append(CPPPATH=[
    "enkido/cedar/include",
    "enkido/akkado/include",
    "src"
])

# C++20 required
env.Append(CXXFLAGS=["-std=c++20"])

# Collect sources
sources = []

# Cedar sources
for root, dirs, files in os.walk("enkido/cedar/src"):
    for file in files:
        if file.endswith(".cpp"):
            sources.append(os.path.join(root, file))

# Akkado sources
for file in os.listdir("enkido/akkado/src"):
    if file.endswith(".cpp"):
        sources.append(os.path.join("enkido/akkado/src", file))

# Extension sources
for file in os.listdir("src"):
    if file.endswith(".cpp"):
        sources.append(os.path.join("src", file))

# Build shared library
library = env.SharedLibrary(
    "bin/enkido.{}.{}.{}".format(
        env["platform"],
        env["target"],
        env["arch"]
    ),
    source=sources
)

Default(library)
```

### 5.6 Extension Configuration

```gdextension
; addons/enkido/enkido.gdextension

[configuration]
entry_symbol = "enkido_library_init"
compatibility_minimum = "4.2"

[libraries]
linux.debug.x86_64 = "res://bin/enkido.linux.template_debug.x86_64.so"
linux.release.x86_64 = "res://bin/enkido.linux.template_release.x86_64.so"
windows.debug.x86_64 = "res://bin/enkido.windows.template_debug.x86_64.dll"
windows.release.x86_64 = "res://bin/enkido.windows.template_release.x86_64.dll"
macos.debug = "res://bin/enkido.macos.template_debug.framework"
macos.release = "res://bin/enkido.macos.template_release.framework"

[icons]
EnkidoPlayer = "res://addons/enkido/icons/enkido_player.svg"
```

---

## 6. Inspector Plugin

```gdscript
# addons/enkido/enkido_inspector.gd

@tool
extends EditorInspectorPlugin

func _can_handle(object: Object) -> bool:
    return object is EnkidoPlayer

func _parse_property(object: Object, type: Variant.Type, name: String,
                     hint_type: PropertyHint, hint_string: String,
                     usage_flags: int, wide: bool) -> bool:

    if name == "source":
        # Add custom source editor with syntax highlighting
        var editor = preload("res://addons/enkido/source_editor.tscn").instantiate()
        editor.player = object
        add_custom_control(editor)
        return true

    return false

func _parse_end(object: Object) -> void:
    var player := object as EnkidoPlayer
    if player == null:
        return

    # Add parameters section
    add_custom_control(HSeparator.new())

    var params_label = Label.new()
    params_label.text = "Parameters"
    params_label.add_theme_font_size_override("font_size", 14)
    add_custom_control(params_label)

    if not player.compiled:
        var hint = Label.new()
        hint.text = "Compile to see parameters"
        hint.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
        add_custom_control(hint)
        return

    var params = player.get_params()
    for p in params:
        match p.type:
            "continuous":
                var slider = HSlider.new()
                slider.min_value = p.min
                slider.max_value = p.max
                slider.value = player.get_param(p.name)
                slider.value_changed.connect(func(v): player.set_param(p.name, v))
                var hbox = HBoxContainer.new()
                var label = Label.new()
                label.text = p.name
                label.custom_minimum_size.x = 100
                hbox.add_child(label)
                hbox.add_child(slider)
                slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
                add_custom_control(hbox)

            "button":
                var btn = Button.new()
                btn.text = p.name
                btn.button_down.connect(func(): player.trigger_button(p.name))
                add_custom_control(btn)

            "toggle":
                var check = CheckButton.new()
                check.text = p.name
                check.button_pressed = player.get_param(p.name) > 0.5
                check.toggled.connect(func(pressed):
                    player.set_param(p.name, 1.0 if pressed else 0.0, 0))
                add_custom_control(check)

            "select":
                var hbox = HBoxContainer.new()
                var label = Label.new()
                label.text = p.name
                label.custom_minimum_size.x = 100
                hbox.add_child(label)
                var option = OptionButton.new()
                for opt in p.options:
                    option.add_item(opt)
                option.selected = int(player.get_param(p.name))
                option.item_selected.connect(func(idx):
                    player.set_param(p.name, float(idx), 0))
                hbox.add_child(option)
                add_custom_control(hbox)
```

---

## 7. Implementation Phases

### Phase 1: Minimal Viable Extension
1. Set up repository with submodules (godot-cpp, enkido)
2. Create build system (SConstruct)
3. Implement `EnkidoEngine` singleton
4. Implement basic `EnkidoPlayer` with compile/play/stop
5. Implement `EnkidoAudioStream` with AudioStreamGenerator
6. Test with simple oscillator in demo project

**Deliverable:** Play a sine wave from Akkado code in Godot

### Phase 2: Parameter System
7. Expose `get_params()` with metadata from compilation
8. Implement `set_param()` / `get_param()` with EnvMap
9. Implement `trigger_button()` with frame-accurate timing
10. Create basic inspector plugin for parameter UI
11. Test parameter modulation from GDScript

**Deliverable:** Change filter cutoff in real-time from game code

### Phase 3: Sample Loading
12. Implement `EnkidoEngine.load_sample()` using Godot file system
13. Add sample resolution during compilation
14. Support WAV loading (OGG via conversion)
15. Test sample playback

**Deliverable:** Play drum samples triggered from game events

### Phase 4: Hot-Swap & Polish
16. Implement smooth crossfade during recompilation
17. Preserve parameter values across hot-swap
18. Add compile error handling and diagnostics
19. Create .akk resource loader for editor import
20. Improve inspector with source editor

**Deliverable:** Edit code while game runs, hear changes immediately

### Phase 5: Production Quality
21. Switch to custom AudioStream for lower latency (optional)
22. Add documentation and API reference
23. Create demo project with adaptive music example
24. Set up CI/CD for multi-platform builds
25. Performance profiling and optimization

**Deliverable:** Production-ready extension with examples

---

## 8. Demo Project

```
demo/
├── project.godot
├── Main.tscn
├── Main.gd
├── Player.tscn
├── Player.gd
├── audio/
│   ├── theme.akk           # Main background music
│   ├── combat.akk          # Combat music layer
│   └── sfx/
│       ├── jump.akk        # Jump sound effect
│       └── hit.akk         # Damage sound
└── samples/
    ├── kick.wav
    ├── snare.wav
    └── hat.wav
```

### Demo Main.gd

```gdscript
extends Node2D

@onready var music: EnkidoPlayer = $Music
@onready var player: CharacterBody2D = $Player

func _ready():
    # Load samples
    EnkidoEngine.load_sample("kick", "res://samples/kick.wav")
    EnkidoEngine.load_sample("snare", "res://samples/snare.wav")
    EnkidoEngine.load_sample("hat", "res://samples/hat.wav")

    # Start music
    music.source_file = "res://audio/theme.akk"
    music.compile()
    music.play()

func _process(delta):
    # Bind player health to music intensity
    var health_ratio = player.health / player.max_health
    music.set_param("intensity", 1.0 - health_ratio)

    # Bind player speed to filter
    var speed_ratio = player.velocity.length() / player.max_speed
    music.set_param("filter", lerp(500.0, 4000.0, speed_ratio))
```

### Demo theme.akk

```akkado
// Adaptive background music

// Parameters bound from game
intensity = param("intensity", 0, 0, 1)
filter = param("filter", 2000, 100, 8000)

// Drum patterns
kick_pat = seq("x...x...x...x...")
snare_pat = seq("....x.......x...")
hat_pat = seq("..x...x...x...x.")

// Drum sounds
kick = sample("kick") * kick_pat
snare = sample("snare") * snare_pat * 0.7
hat = sample("hat") * hat_pat * 0.4

drums = kick + snare + hat

// Bass synth - responds to filter param
bass_freq = 55 * (1 + intensity * 0.5)  // Pitch rises with intensity
bass = osc("saw", bass_freq) |> lpf(%, filter, 0.3)

// Pad - fades in with intensity
pad_freq = 220
pad = osc("triangle", pad_freq) |> lpf(%, 1000)
pad = pad * intensity * 0.3

// Mix
master = drums * 0.8 + bass * 0.4 + pad
master |> out(%, %)
```

---

## 9. Testing Strategy

### Unit Tests (C++)

```cpp
// tests/test_enkido_player.cpp

TEST_CASE("EnkidoPlayer compiles valid source") {
    EnkidoPlayer player;
    player.set_source("osc(\"saw\", 220) |> out(%, %)");

    bool success = player.compile();
    REQUIRE(success);
    REQUIRE(player.is_compiled());
}

TEST_CASE("EnkidoPlayer extracts parameters") {
    EnkidoPlayer player;
    player.set_source(R"(
        vol = param("volume", 0.8, 0, 1)
        osc("saw", 220) * vol |> out(%, %)
    )");

    player.compile();

    Array params = player.get_params();
    REQUIRE(params.size() == 1);

    Dictionary p = params[0];
    CHECK(String(p["name"]) == "volume");
    CHECK(String(p["type"]) == "continuous");
    CHECK(float(p["default"]) == Approx(0.8f));
}

TEST_CASE("EnkidoPlayer set_param updates EnvMap") {
    EnkidoPlayer player;
    player.set_source(R"(
        vol = param("volume", 0.5)
        dc(1) * vol |> out(%, %)
    )");

    player.compile();
    player.set_param("volume", 0.75);

    float value = player.get_param("volume");
    CHECK(value == Approx(0.75f));
}
```

### Integration Tests (GDScript)

```gdscript
# tests/test_enkido.gd

extends GutTest

func test_compile_and_play():
    var player = EnkidoPlayer.new()
    add_child(player)

    player.source = "osc('saw', 220) |> out(%, %)"
    assert_true(player.compile())

    player.play()
    assert_true(player.is_playing())

    await get_tree().create_timer(0.1).timeout

    player.stop()
    assert_false(player.is_playing())

    player.queue_free()

func test_parameter_binding():
    var player = EnkidoPlayer.new()
    add_child(player)

    player.source = """
        vol = param("volume", 0.5, 0, 1)
        osc("saw", 220) * vol |> out(%, %)
    """
    player.compile()

    var params = player.get_params()
    assert_eq(params.size(), 1)
    assert_eq(params[0].name, "volume")

    player.set_param("volume", 0.8)
    assert_almost_eq(player.get_param("volume"), 0.8, 0.01)

    player.queue_free()
```

---

## 10. Open Questions

1. **Web export:** Can we compile to WASM for HTML5 exports?
2. **Mobile:** What's needed for Android/iOS support?
3. **Visual scripting:** Should there be a node-based editor integration?
4. **Godot 3.x:** Is backwards compatibility worth supporting?
5. **Audio buses:** Should players auto-connect to specific buses?
6. **Spatial audio:** How should 3D positioning work?

---

## 11. Success Metrics

- Extension builds on Windows, Linux, macOS
- Demo project runs without audio dropouts
- Hot-swap updates audio within 50ms
- Parameter changes take effect within 20ms
- Memory usage stable over extended sessions
- Inspector UI updates reflect parameter changes

---

## 12. Timeline Estimate

| Phase | Duration | Deliverable |
|-------|----------|-------------|
| Phase 1: MVP | 1-2 weeks | Play sine wave in Godot |
| Phase 2: Parameters | 1 week | Parameter binding works |
| Phase 3: Samples | 1 week | Sample playback works |
| Phase 4: Hot-Swap | 1 week | Live coding works |
| Phase 5: Polish | 1-2 weeks | Production ready |
| **Total** | **5-8 weeks** | Full featured extension |
