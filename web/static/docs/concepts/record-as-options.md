---
title: Records as Builtin Options
category: concepts
order: 4
keywords: [options, record, builtin, convention, OptionSchema, params, configuration, viz, autocomplete]
---

# Records as Builtin Options

When a builtin needs more parameters than fit comfortably in positional slots, akkado uses a **record literal as the last positional argument**. This convention keeps call sites readable, lets the editor offer field-name autocomplete, and gives the compiler a typed schema to validate against.

## The pattern

```akkado
// Last positional argument is a record. Each field is a named option.
osc("saw", 220) |> waterfall(%, "scope", {
    fft: 1024,
    gradient: "viridis",
    angle: 270,
}) |> out(%, %)
```

Three pieces have to line up:

1. The builtin declares a `Record`-typed parameter slot in its signature.
2. The builtin attaches an `OptionSchema` to that slot — one entry per legal field, each with a name, type, default, and description.
3. The shared codegen helper `extract_options(arena, node, schema)` turns the caller's record literal into a typed payload, dropping any field name not in the schema.

The editor reads the schema through `akkado_get_builtins_json()` and offers field-name completions inside the record literal as you type.

## Why this shape

- **Names instead of positions.** Once a builtin grows past three or four parameters, positional ordering becomes a memorization burden. `waterfall(sig, "x", 1024, "viridis", 270, 40, -90, 0)` is unreadable; the record version names every value.
- **Defaults come from the schema.** The caller writes only the fields they want to override. Missing fields fall back to whatever the schema declares.
- **One arity, many futures.** New options can be added to the schema without changing the call signature.
- **Editor visibility.** Phase 1 of the records-system work wired the schema through to autocomplete; no per-builtin client code is needed.

## Authoring a builtin with options

Declare the schema next to the builtin's `BuiltinInfo`. The slot index identifies which positional parameter is record-shaped (commonly the last one).

```cpp
// akkado/include/akkado/builtins.hpp
{"waterfall", {.opcode = cedar::Opcode::COPY,
               .input_count = 1, .optional_count = 2,
               .param_names = {"signal", "name", "options", "", "", ""},
               .param_types = {ParamValueType::Signal,
                               ParamValueType::String,
                               ParamValueType::Record},
               .option_schemas = {OptionSchema{
                   /*param_index=*/2,
                   /*fields=*/{{
                       {"fft",      OptionFieldType::Enum,   "1024",
                        "FFT bin count", "256,512,1024,2048"},
                       {"gradient", OptionFieldType::Enum,   "\"magma\"",
                        "Color gradient", "magma,viridis,inferno,grayscale"},
                       {"angle",    OptionFieldType::Number, "180",
                        "Scroll direction in degrees"},
                       // ...
                   }},
                   /*field_count=*/8,
               }},
               .option_schema_count = 1}},
```

Each `OptionField` carries:

- `name` — the field name as it appears in source.
- `type` — `Number`, `String`, `Bool`, or `Enum`.
- `default_repr` — textual default as it would appear in source (e.g. `"180"`, `"\"viridis\""`, `"true"`). The empty string means "no default".
- `description` — one-line tooltip surfaced by the editor.
- `enum_values` — comma-separated allowed values, only meaningful when `type == Enum`.

The builtin handler then reads the caller's record through the shared helper:

```cpp
#include "akkado/codegen/options.hpp"

const OptionSchema* schema = info->find_option_schema(/*param_index=*/2);
auto options = codegen::extract_options(ast_->arena, options_arg,
                                         schema ? *schema : OptionSchema{});

// Typed access — returns nullopt when the caller did not supply the field.
auto fft  = options.get_number("fft");
auto grad = options.get_string("gradient");

// Forward the canonical JSON to the web UI / metadata sink.
decl.options_json = options.to_json();
```

`OptionsPayload::unknown_fields` collects the names of any caller-supplied fields not declared on the schema. They are dropped from the JSON today; a future pass will surface them as a `W160` warning once that diagnostic ships from the spread PRD.

## Recommended families to adopt

Visualizers ship the convention today. Three more families are good candidates for follow-up adoption — each warrants its own per-family PRD because the field shape and back-end plumbing differ:

| Family | Builtins | Likely option fields |
|---|---|---|
| Visualizers | `pianoroll`, `oscilloscope`, `waveform`, `spectrum`, `waterfall` | already adopted |
| Samplers | `samples`, `sample_play` | `bits`, `sr`, `preset`, `loop_mode`, `pitch_algo`, `grain_size`, `grain_overlap`, `grain_window`, `grain_jitter` |
| Filters | `lp`, `hp`, `bp`, `moog`, `svflp`, … | `q`, `dry`, `wet`, `oversample` |
| Delays / reverbs | `delay`, `comb`, `freeverb`, `dattorro`, `lexicon` | `feedback`, `dry`, `wet`, `mode`, `damping`, `size` |

The convention does not change the call signature for already-shipped builtins (samplers, filters, delays still use positional params). Migration to the options form is opt-in per family.

## Composes with spread (when it ships)

Once `prd-record-argument-spread.md` lands, callers can build option presets and reuse them:

```akkado
// Future — pending the spread PRD.
default_scope = {fft: 1024, gradient: "viridis", angle: 270}
osc("saw", 220) |> waterfall(%, "left",  ..default_scope)              |> out(%, %)
osc("saw", 110) |> waterfall(%, "right", ..default_scope, angle: 90)   |> out(%, %)
```

The `accepts_spread` bit on `OptionSchema` already marks each schema as spread-compatible; the editor will surface remaining unfilled options once the syntax is live.

## See also

- [Records reference](../reference/language/records.md) — record literals, field access, destructuring, and state cells.
- [State cells](../reference/builtins/state.md) — record-valued state cells with `cell.field` sugar.
