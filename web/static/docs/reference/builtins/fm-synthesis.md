---
title: FM Synthesis
category: builtins
order: 14
keywords: [fm, frequency, modulation, carrier, modulator, ratio, index, sideband, harmonic, synthesis]
group: instruments
subgroup: synthesis
icon: Radio
tagline: Carrier/modulator frequency modulation with rational ratios.
---

# FM Synthesis

Frequency modulation (FM) synthesizes complex timbres by modulating the frequency of one oscillator (the **carrier**) with the output of another (the **modulator**). nkido has no dedicated `fm()` builtin. You build FM compositionally: feed a modulator signal into the frequency input of `osc()`.

## carrier

The **carrier** is the oscillator whose pitch you hear. It runs at the perceived musical frequency.

```akk
// Sine carrier at 440 Hz, no modulation yet
osc("sin", 440) |> out(@)
```

## modulator

The **modulator** is a second oscillator whose output is added to the carrier's frequency. The modulator's amplitude (the **modulation index**) controls how far the carrier's frequency swings.

```akk
// Slow vibrato. Modulator at 5 Hz, ±10 Hz swing
osc("sin", 440 + osc("sin", 5) * 10) |> out(@)
```

## index

The **modulation index** is the modulator's amplitude. Low values give subtle vibrato; high values create rich, harmonic-dense tones characteristic of classic FM (DX7-style bell, electric piano, brass).

```akk
// Bell-like timbre with a high modulation index
osc("sin", 440 + osc("sin", 660) * 200) |> out(@)
```

## ratio

The **frequency ratio** between modulator and carrier determines harmonic content. Integer ratios (1:1, 2:1, 3:1) produce harmonic spectra; non-integer ratios (1.41:1, 1.618:1) produce inharmonic, bell- and gong-like timbres.

```akk
// 2:1 ratio carrier:modulator (classic harmonic FM)
freq = 220
osc("sin", freq + osc("sin", freq * 2) * 100) |> out(@)
```

```akk
// Inharmonic ratio for a bell
freq = 220
osc("sin", freq + osc("sin", freq * 1.41) * 300) |> out(@)
```

## sideband

FM produces **sidebands** at carrier ± n·modulator for integer n. The modulation index controls how many sidebands are audible: higher index, more sidebands, brighter tone. That's why a single carrier can move from pure-tone vibrato (low index) to bright, edgy timbres (high index) without changing pitch.

```akk
// FM using phasor as modulator (sawtooth-shaped sweep)
osc("sin", 440 + osc("phasor", 5) * 100) |> out(@)
```

## harmonic

For **harmonic** FM, the modulator's frequency is an integer multiple of the carrier's. The result has only harmonic sidebands and stays pitched.

```akk
// Harmonic FM with modulator at 2x carrier frequency
osc("sin", 110 + osc("sin", 220) * 80) |> out(@)
```

Related: [oscillators](oscillators), [modulation](modulation), [math](math)
