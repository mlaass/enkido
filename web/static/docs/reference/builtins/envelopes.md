---
title: Envelopes
category: builtins
subcategory: envelopes
keywords: [envelope, adsr, ar, attack, decay, sustain, release, gate, trigger]
---

# Envelopes

Envelopes shape the amplitude or other parameters of a sound over time. They respond to gates (sustained signals) or triggers (momentary pulses).

## adsr

**ADSR Envelope** - Attack, Decay, Sustain, Release envelope.

| Param   | Type    | Default | Description |
|---------|---------|---------|-------------|
| gate    | trigger | -       | Gate signal (1 = on, 0 = off) |
| attack  | number  | 0.01    | Attack time in seconds |
| decay   | number  | 0.1     | Decay time in seconds |

Aliases: `envelope`

The classic ADSR envelope. When the gate goes high, the envelope attacks to full level, decays to the sustain level, and stays there until the gate goes low, when it releases to zero.

```akk
// Basic synth voice with ADSR
sin(440) * adsr(trigger(2), 0.01, 0.2) |> out(%, %)
```

```akk
// Plucky sound (short attack and decay)
saw(330) * adsr(trigger(4), 0.001, 0.1) |> lp(%, 2000) |> out(%, %)
```

```akk
// Pad sound (slow attack)
saw(220) * adsr(trigger(0.5), 0.5, 0.3) |> lp(%, 1000) |> out(%, %)
```

Related: [ar](#ar), [trigger](#trigger)

---

## ar

**AR Envelope** - Attack, Release envelope.

| Param   | Type    | Default | Description |
|---------|---------|---------|-------------|
| trig    | trigger | -       | Trigger signal |
| attack  | number  | 0.01    | Attack time in seconds |
| release | number  | 0.3     | Release time in seconds |

A simpler two-stage envelope. On trigger, attacks to full level then immediately releases. Good for percussive sounds.

```akk
// Kick drum
sin(55) * ar(trigger(4), 0.001, 0.2) |> out(%, %)
```

```akk
// Hi-hat
noise() |> hp(%, 8000) * ar(trigger(8), 0.001, 0.05) |> out(%, %)
```

```akk
// Pluck
saw(440) * ar(trigger(4), 0.001, 0.1) |> lp(%, 2000) |> out(%, %)
```

Related: [adsr](#adsr), [trigger](#trigger)
