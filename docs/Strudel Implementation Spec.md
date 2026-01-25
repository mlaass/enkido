# **Strudel / TidalCycles Mini-Notation & Sampler Implementation Specification**

## **1\. Overview**

Strudel relies on a concise domain-specific language (DSL) known as "Mini-Notation" to define rhythmic patterns and musical events within a continuous cycle of time. Unlike linear sequencers, Strudel divides a fixed time cycle (the "cycle") into smaller subdivisions based on the number of events.

This document outlines the grammar (EBNF), semantic logic, and sampler API required to implement this system.

## **2\. Mini-Notation Grammar (EBNF)**

The following EBNF describes the parsing rules for a Mini-Notation string.

**Core Concept:** The parser must treat whitespace as a sequential separator and commas as parallel separators.

(\* Root Pattern \*)
PatternString   ::= Sequence

(\* Sequences and Parallelism \*)
Sequence        ::= Step { Whitespace Step }
Step            ::= Polyphony { Modifier }
Polyphony       ::= Element { "," Element }

(\* Base Elements \*)
Element         ::= Rest
                  | Literal
                  | Subdivision
                  | Alternation
                  | RandomChoice

(\* Grouping Structures \*)
Subdivision     ::= "\[" Sequence "\]"         (\* Divides current step duration by N steps \*)
Alternation     ::= "\<" Sequence "\>"         (\* Selects one step per cycle, advancing sequentially \*)
RandomChoice    ::= Element "|" Element      (\* Randomly selects one of the elements \*)

(\* Values \*)
Literal         ::= Identifier | NoteName | SampleSelector
Rest            ::= "\~" | "\_"
Identifier      ::= \[a-zA-Z0-9\#\]+
SampleSelector  ::= Identifier ":" Digit+    (\* e.g., hh:4 \*)

(\* Modifiers (Applied right-to-left or left-to-right depending on implementation preference, usually postfix) \*)
Modifier        ::= SpeedMod
                  | ElongationMod
                  | ReplicationMod
                  | EuclideanMod
                  | ProbabilityMod

SpeedMod        ::= "\*" Number               (\* Fast: Play N times within the step \*)
                  | "/" Number               (\* Slow: Stretch over N steps \*)
ElongationMod   ::= "@" Number               (\* Weight: Takes up the space of N steps \*)
ReplicationMod  ::= "\!" Number               (\* Repeat: Repeat N times (no speed change) \*)
ProbabilityMod  ::= "?" \[Number\]             (\* Chance: 50% or specific probability of dropping \*)

(\* Euclidean Rhythm: (beats, segments, offset) \*)
EuclideanMod    ::= "(" Integer "," Integer \[ "," Integer \] ")"

(\* Basic Types \*)
Number          ::= Digit+ \[ "." Digit+ \]
Integer         ::= Digit+
Whitespace      ::= " " | "\\t" | "\\n"

## **3\. Semantic Implementation Guide**

### **3.1. Time Division (The Cycle)**

The fundamental rule of the Mini-Notation is that **adding events divides the cycle**.

* "a b": Cycle is split into 2 halves. a plays at 0.0, b plays at 0.5.
* "a b c": Cycle is split into 3 thirds. a at 0.0, b at 0.33, c at 0.66.

### **3.2. Grouping Logic**

* **Subdivision \[\]**: The bracketed group occupies the duration of a *single* step in the parent sequence.
  * *Input:* note("a \[b c\] d")
  * *Logic:* 3 main steps (a, group, d). 'a' and 'd' are 1/3 cycle. 'b' and 'c' share the middle 1/3 (so they are 1/6 cycle each).
* **Alternation \<\>**: The bracketed group advances one index per cycle.
  * *Input:* note("a \<b c\>")
  * *Cycle 1:* Plays a b
  * *Cycle 2:* Plays a c
  * *Cycle 3:* Plays a b

### **3.3. Modifiers**

| Symbol | Name | Logic | Example |
| :---- | :---- | :---- | :---- |
| \* | Speed Up | Repeats the event ![][image1] times within its allocated slot. | bd\*2 → bd bd (in space of one) |
| / | Slow Down | Stretches the event. Only complete segments play if cycle \< duration. | bd/2 → plays bd over 2 cycles |
| \! | Replication | Repeats event without compressing time (extends sequence). | bd\!2 \== bd bd |
| @ | Elongation | Increases the relative weight of the step in the sequence. | a@2 b → a is 2/3, b is 1/3 |
| ? | Degrade | Randomly silences the event based on float probability (0.0-1.0). Default 0.5. | bd? |
| (k,n,o) | Euclidean | Bjorklund algorithm. Distributes ![][image2] hits over ![][image3] steps, rotated by ![][image4]. | bd(3,8) |

### **3.4. Polyphony**

* **Comma ,**: Events separated by commas happen simultaneously (start at the same time offset).
* *Input:* \[a, b\] c
* *Logic:* Step 1 contains both a and b. Step 2 contains c.

### **3.5. Pattern Object Model & Chaining**

A critical architectural concept is that **Patterns are first-class objects**, not just static strings. Functions like s(), note(), and chord() parse the Mini-Notation string and return a **Pattern Object**.

* **Fluent Interface:** Pattern objects expose methods (effects, transformations) that return a new, modified Pattern Object. This allows for method chaining.
* **Pattern-as-Parameter:** Arguments to these methods can themselves be Mini-Notation patterns, not just static values.

**Example:**

// The filter cutoff frequency (.lpf) is itself a pattern that evolves over cycles
s("bd sd \[\~ bd\] sd,hh\*6").lpf("\<4000 2000 1000 500 200 100\>")

## **4\. Sampler Architecture**

The sampler implementation requires a **Sample Map** (alias to URL) and a **Buffer Manager** (audio data).

### **4.1. Loading Strategy**

* **Lazy Loading:** Audio buffers are fetched via HTTP only when the pattern triggers the note.
* **Formats:** .wav, .mp3, .ogg.
* **Strudel JSON:** A standard schema for mapping sample names to file paths.
  {
    "\_base": "\[https://path.to/samples/\](https://path.to/samples/)",
    "bd": "bd/kick.wav",
    "sd": \["sd/snare1.wav", "sd/snare2.wav"\] // Round-robin or indexable array
  }

## **5\. Functional Pattern Constructors**

Strudel treats patterns as first-class objects. While the Mini-Notation string is the primary input method, the following functions provide a programmatic API to construct patterns. These functions are often chainable and composable.

### **5.1. Equivalence Table**

| Functional API | Mini-Notation | Description |
| :---- | :---- | :---- |
| **cat(x, y)** | \<x y\> | **Slow Concatenation**: Each item takes one full cycle. |
| **seq(x, y)** | "x y" | **Fast Concatenation**: Items are squashed to fit into one cycle. |
| **stack(x, y)** | "x,y" | **Polyphony**: Items play simultaneously with the same duration. |
| **stepcat(\[3,x\],\[2,y\])** | "x@3 y@2" | **Weighted Concatenation**: Items have proportional lengths. |
| **polymeter(x, y)** | "{x y}" | **Polymeter**: Steps align; patterns repeat to fit the lowest common multiple duration. |
| **polymeterSteps(n, x)** | "{x}%n" | **Phasing**: Forces pattern x to span n steps. |
| **silence** | "\~" | **Rest**: A pattern containing no events. |

### **5.2. Constructor details**

* **cat(...patterns) / slowcat**
  concatenates patterns sequentially. If cat has 4 arguments, the resulting pattern will repeat every 4 cycles.
  * *Example:* s("hh\*4").cat(note("c"), note("d")) ![][image5] Cycle 1: C, Cycle 2: D.
* **seq(...patterns) / fastcat**
  Concatenates patterns within a single cycle. The duration of each input pattern is compressed.
  * *Example:* seq("e5", "b4", \["d5", "c5"\]) is equivalent to note("e5 b4 \[d5 c5\]").
* **stack(...patterns) / polyrhythm**
  Layers patterns on top of each other.
  * *Example:* stack("bd", "hh\*2") plays a kick and 2 hi-hats concurrently.
* **stepcat(...weightedPairs) / timecat**
  Concatenates patterns with explicit time weights.
  * *Input:* stepcat(\[3, "a"\], \[1, "b"\])
  * *Logic:* Total weight \= 4\. "a" takes 3/4 of the cycle. "b" takes 1/4.
* **arrange(...layoutPairs)**
  Sequences patterns over specific cycle counts.
  * *Input:* arrange(\[4, patternA\], \[2, patternB\])
  * *Logic:* Plays patternA for 4 cycles, then patternB for 2 cycles. Total loop length \= 6 cycles.

### **5.3. Algorithmic Generators**

* **run(n)**
  Generates a discrete integer pattern from 0 to n-1. Useful for indexing samples.
  * *Example:* n(run(4)) ![][image6] n("0 1 2 3").
* **binary(integer)**
  Converts a number to its binary representation and returns it as a boolean pattern.
  * *Example:* binary(5) (![][image7]) ![][image5] struct("1 0 1").
* **binaryN(integer, bits)**
  Same as binary, but padded to a fixed bit-length (e.g., 8 or 16 bits).

## **6\. Time & Structure Modifiers**

These functions manipulate the temporal structure or "flow" of patterns. They can be chained onto existing pattern objects.

### **6.1. Speed & Tempo**

| Function | Mini-Notation | Description |
| :---- | :---- | :---- |
| **slow(factor)** | / | Slows down the pattern by factor. slow(2) stretches 1 cycle to 2\. |
| **fast(factor)** | \* | Speeds up the pattern by factor. fast(2) plays the pattern twice in 1 cycle. |
| **fastGap(factor)** | \- | Speeds up pattern but leaves silence for the rest of the cycle. fastGap(2) plays pattern in 1st half, silence in 2nd. |
| **cpm(rate)** | \- | Sets "Cycles Per Minute". Defines the absolute tempo of the pattern. |

### **6.2. Offsetting & Looping**

| Function | Description |
| :---- | :---- |
| **early(cycles)** | Shifts pattern backwards in time (nudge left). Equivalent to Tidal \<\~. |
| **late(cycles)** | Shifts pattern forwards in time (nudge right). Equivalent to Tidal \~\>. |
| **ribbon(offset, cycles)** | Captures a loop of length cycles starting at offset (in cycles) and repeats it forever. |

### **6.3. Iteration & Reordering**

| Function | Description |
| :---- | :---- |
| **rev()** | Reverses the events in the cycle. |
| **palindrome()** | Alternates between forward and reverse playback every cycle. |
| **iter(n)** | Slices pattern into n parts, shifting the start slice by 1 every cycle. |
| **iterBack(n)** | Like iter, but plays the subdivisions in reverse order (Tidal iter'). |
| **ply(reps)** | Repeats each event reps times within its original duration. |

### **6.4. Euclidean & Geometric**

| Function | Mini-Notation | Description |
| :---- | :---- | :---- |
| **euclid(k, n)** | (k,n) | Distributes k pulses over n steps using Bjorklund's algorithm. |
| **euclidRot(k, n, rot)** | (k,n,rot) | Same as euclid, rotated by rot steps. |
| **euclidLegato(k, n)** | \- | Euclidean rhythm where notes are held until the next pulse (no gaps). |

### **6.5. Segmentation & Time Warping**

| Function | Description |
| :---- | :---- |
| **segment(n)** | Samples the pattern at n discrete points per cycle. Turns continuous patterns into discrete events. |
| **compress(start, len)** | Squeezes the full cycle into a smaller window defined by start (0-1) and length. |
| **zoom(start, end)** | Plays only the portion of the pattern between start and end, stretching it to fill the cycle. |
| **linger(fraction)** | Plays the first fraction of the pattern, then repeats that part to fill the rest of the cycle. |
| **swing(n)** | Shorthand for swingBy(1/3, n). Creates a shuffle feel. |
| **swingBy(amt, n)** | Breaks cycle into n slices and delays the second half of each slice by amt (0-1). |
| **clip(n) / legato(n)** | Multiplies duration of events. 1=legato, \<1=staccato, \>1=overlap. |

### **6.6. Scope Modifiers**

| Function | Description |
| :---- | :---- |
| **inside(n, func)** | Applies func while the pattern is temporarily sped up by n. Conceptually: slow(n).func().fast(n). |
| **outside(n, func)** | Applies func while the pattern is temporarily slowed down by n. Conceptually: fast(n).func().slow(n). |

## **7\. Harmony & Voicing System**

Strudel provides an abstraction layer for handling chords, voicings, and voice leading. This system separates the *definition* of a chord progression from its specific *realization* (voicing).

### **7.1. Chord Definition**

A **Chord** is defined as a set of intervals relative to a root note.

* **Triads**: The most common building blocks.
  * **Major**: \[0, 4, 7\]
  * **Minor**: \[0, 3, 7\]
  * **Diminished**: \[0, 3, 6\]
  * **Augmented**: \[0, 4, 8\]

### **7.2. The chord() Function**

Parses chord symbol strings and converts them into pitch clusters.

* **Input**: A pattern of chord symbol strings (e.g., chord("Am C7 Dm")).
* **Structure**: Symbols are typically {Root}{Quality}{Extensions}.
  * **Root**: Note name (C, D\#, Eb).
  * **Quality**:
    * m, \- (Minor)
    * M, ^, maj (Major)
    * o, dim (Diminished)
    * aug, \+ (Augmented)
    * sus (Suspended)
  * **Extensions**: 7, 9, 11, 13, 6, 69, etc.
* **Behavior**: Uses a **Voicing Dictionary** to look up intervals for the symbol and applies them to the root.

### **7.3. The voicing() Function**

Algorithms for **Voice Leading**—arranging the notes of chords to minimize distance between transitions.

* **Purpose**: Automatically rearranges notes (inversions, octaves) to ensure smooth playback.
* **Logic**:
  1. Retrieve intervals for the chord.
  2. Adjust octaves of individual notes based on the anchor and mode.
  3. Minimize the total interval distance from the previous chord state (if applicable).

### **7.4. Voicing Controls**

These functions modify how voicing() generates notes.

| Function | Parameters | Description |
| :---- | :---- | :---- |
| **anchor** | Note / MIDI | Sets the target "center of gravity" for the voicing. The algorithm tries to keep notes close to this pitch. Default is typically c5 (MIDI 72). |
| **mode** | string | Constrains the voicing relative to the anchor. • **below**: Top note ![][image8] anchor (default). • **above**: Bottom note ![][image9] anchor. • **duck**: Top note ![][image10] anchor. • **root**: Bottom note is always the root. |
| **dict** | string | Selects a specific voicing dictionary (e.g., 'default', 'jazz', or user-defined). |
| **addVoicings** | name, map | Registers a custom dictionary. Maps chord symbols to list of interval strings (e.g., {'m': \['0 3 7'\]}). |