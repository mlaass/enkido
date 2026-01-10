# Akkado: Language Specification v2

Akkado is a domain-specific language for live-coding musical patterns and modular synthesis. It combines Strudel/Tidal-style mini-notation with a functional DAG approach to audio signal processing.

## 1. Core Philosophy

- **The Pattern is the Trigger:** Patterns (via mini-notation) produce control signals (triggers, velocities, pitches).
- **The Closure is the Voice:** Anonymous functions bridge control data and audio synthesis.
- **The Pipe is the Edge:** The `|>` operator defines signal flow through the DAG.
- **The Hole is the Port:** The `%` symbol is an explicit input port for signal injection.

## 2. Lexical Structure

### 2.1 Whitespace and Comments

- Whitespace (space, tab, newline) separates tokens but is otherwise ignored.
- Line comments start with `//` and extend to end of line.
- No block comments.
- No semicolons.

### 2.2 Identifiers and Keywords

```ebnf
identifier = letter { letter | digit | "_" } ;
letter     = "a"..."z" | "A"..."Z" | "_" ;
digit      = "0"..."9" ;
```

**Reserved Keywords:**
```
true  false  post  pat  seq  timeline  note
```

**Built-in Functions** (resolved at semantic analysis, not parsing):

Built-in functions are parsed as regular function calls. The semantic analyzer resolves them to VM opcodes. This list is extensible without grammar changes.

| Function | Signature | Description |
|----------|-----------|-------------|
| **Oscillators** | | |
| `sin` | `sin(freq)` | Sine oscillator |
| `tri` | `tri(freq)` | Triangle oscillator |
| `saw` | `saw(freq)` | Sawtooth oscillator |
| `sqr` | `sqr(freq)` | Square oscillator |
| `ramp` | `ramp(freq)` | Inverted sawtooth |
| `phasor` | `phasor(freq)` | Phase accumulator (0-1) |
| `noise` | `noise()` | White noise |
| **Filters** | | |
| `lp` | `lp(in, cut, q=0.707)` | SVF lowpass filter |
| `hp` | `hp(in, cut, q=0.707)` | SVF highpass filter |
| `bp` | `bp(in, cut, q=0.707)` | SVF bandpass filter |
| `moog` | `moog(in, cut, res=1.0)` | Moog ladder filter (4-pole) |
| **Envelopes** | | |
| `adsr` | `adsr(gate, attack=0.01, decay=0.1)` | ADSR envelope |
| `ar` | `ar(trig, attack=0.01, release=0.3)` | Attack-release envelope |
| **Delays** | | |
| `delay` | `delay(in, time, fb)` | Delay line (time in ms) |
| **Math** | | |
| `add`, `sub`, `mul`, `div`, `pow` | `op(a, b)` | Arithmetic (from operators) |
| `neg`, `abs`, `sqrt`, `log`, `exp` | `f(x)` | Unary math functions |
| `floor`, `ceil` | `f(x)` | Rounding functions |
| `min`, `max` | `f(a, b)` | Min/max of two values |
| `clamp` | `clamp(x, lo, hi)` | Clamp to range |
| `wrap` | `wrap(x, lo, hi)` | Wrap to range |
| **Utility** | | |
| `mtof` | `mtof(note)` | MIDI to frequency |
| `slew` | `slew(target, rate)` | Slew rate limiter |
| `sah` | `sah(in, trig)` | Sample and hold |
| `out` | `out(signal)` or `out(L, R)` | Output to speakers |
| **Timing** | | |
| `clock` | `clock()` | Beat phase (0-1) |
| `lfo` | `lfo(rate, duty=0.5)` | Beat-synced LFO |
| `trigger` | `trigger(div)` | Beat-division trigger |
| `euclid` | `euclid(hits, steps, rot=0)` | Euclidean rhythm |

Aliases: `sine`→`sin`, `triangle`→`tri`, `sawtooth`→`saw`, `square`→`sqr`, `lowpass`→`lp`, `highpass`→`hp`, `bandpass`→`bp`, `svflp`→`lp`, `svfhp`→`hp`, `svfbp`→`bp`

### 2.3 Literals

**Numbers:**
```ebnf
number = [ "-" ] digit { digit } [ "." digit { digit } ] ;
```
The lexer consumes `-` as part of the number only when immediately followed by a digit with no intervening whitespace. Otherwise `-` is a separate token.

Examples:
- `42` — integer
- `3.14` — float
- `-1` — negative number (single token)
- `x - 1` — subtraction (three tokens)
- `x * -1` — multiply by negative (four tokens: `x`, `*`, `-1`)
- `x * -y` — requires `neg()`: write `x * neg(y)`

**Booleans:**
```ebnf
bool_literal = "true" | "false" ;
```

**Strings:**
```ebnf
string = quote { any_char | escape_seq } quote ;
quote  = '"' | "'" | "`" ;
```
All three quote types are equivalent. Strings may span multiple lines.

Escape sequences: `\\`, `\"`, `\'`, `` \` ``, `\n`, `\t`, `\r`

**Pitch Literals** (outside mini-notation):
```ebnf
pitch_literal = "'" pitch_name octave "'" ;
pitch_name    = ( "a"..."g" | "A"..."G" ) [ "#" | "b" ] ;
octave        = digit ;
```
Octave is **required** outside mini-notation.

Examples: `'c4'`, `'f#3'`, `'Bb5'`

**Chord Literals:**
```ebnf
chord_literal = "'" pitch_name octave ":" chord_type "'" ;
chord_type    = "maj" | "min" | "dom7" | "maj7" | "min7" | "dim" | "aug" | "sus2" | "sus4" ;
```

Examples: `'c4:maj'`, `'a3:min7'`

### 2.4 Operators and Delimiters

**Operators** (in precedence order, highest first):
| Token | Name | Desugars To |
|-------|------|-------------|
| `.` | Method call | (special syntax) |
| `^` | Power | `pow(a, b)` |
| `*` `/` | Multiply, Divide | `mul(a, b)`, `div(a, b)` |
| `+` `-` | Add, Subtract | `add(a, b)`, `sub(a, b)` |
| `|>` | Pipe | (signal flow) |

**Other Tokens:**
```
(  )  [  ]  {  }  ,  :  %  ->  =
```

## 3. Grammar

### 3.1 Program Structure

```ebnf
program    = { statement } ;
statement  = assignment | post_stmt | pipe_expr ;
assignment = identifier "=" pipe_expr ;
post_stmt  = "post" "(" closure ")" ;
```

Statements are separated by newlines or simply sequenced. No semicolons.

### 3.2 Expressions — Precedence Hierarchy

From lowest to highest precedence:

```ebnf
pipe_expr   = add_expr { "|>" add_expr } ;
add_expr    = mul_expr { ( "+" | "-" ) mul_expr } ;
mul_expr    = pow_expr { ( "*" | "/" ) pow_expr } ;
pow_expr    = method_expr { "^" method_expr } ;
method_expr = primary { "." identifier "(" [ arg_list ] ")" } ;
primary     = atom | "(" pipe_expr ")" ;
```

**Key Rule:** Pipes (`|>`) are the lowest precedence. The `%` hole references the left-hand side value. Pipes can appear anywhere an expression is valid, including function arguments and closure bodies.

### 3.3 Atoms

```ebnf
atom = number
     | bool_literal
     | string
     | pitch_literal
     | chord_literal
     | identifier
     | hole
     | function_call
     | closure
     | mini_literal ;

hole = "%" ;
```

### 3.4 Function Calls

```ebnf
function_call = identifier "(" [ arg_list ] ")" ;
arg_list      = argument { "," argument } ;
argument      = [ identifier ":" ] pipe_expr ;
```

Named arguments use `name: value` syntax. Follows Python conventions:
- Positional arguments must precede named arguments
- Named arguments can appear in any order
- Each parameter can only be specified once
- Named args allow skipping optional params with defaults

Examples:
```
saw(440)                           // positional only
lp(%, 1000, 0.7)                   // positional only
lp(in: %, cut: 1000)               // named only (q uses default 0.707)
ar(gate, release: 0.5)             // mixed: 1 positional, 1 named
svflp(in: %, cut: 800, q: 0.5)     // all named
```

### 3.5 Method Calls

```ebnf
method_call = primary "." identifier "(" [ arg_list ] ")" ;
```

Methods bind tighter than all binary operators. Methods chain left-to-right.

Examples:
```
p.map(hz -> saw(hz))
signal.map(f).filter(g).take(4)
%.map(x -> x * 0.5)
```

### 3.6 Closures

```ebnf
closure        = "(" [ param_list ] ")" "->" closure_body ;
param_list     = param { "," param } ;
param          = identifier [ "=" number ] ;
closure_body   = block | pipe_expr ;
block          = "{" { statement } [ pipe_expr ] "}" ;
```

**Closure Body Rule:** The closure body is "greedy" — it captures everything including pipes.

```
(x) -> x + 1              // body is "x + 1"
(x) -> x |> f(%)          // body is "x |> f(%)" → rewrites to f(x)
(x) -> x + 1 |> f(%)      // body is "x + 1 |> f(%)" → rewrites to f(x + 1)
((x) -> x + 1) |> f(%)    // parens needed to pipe the closure itself
(x) -> { ... }            // body is the block
```

**Block Return:** The last expression in a block is the implicit return value.

```
(x) -> {
    y = x + 1
    y * 2         // this is returned
}
```

### 3.6.1 Closure Default Parameters

Parameters can have default values using `=` syntax:

```
(x, q=0.707) -> lp(x, 1000, q)
(sig, attack=0.01, release=0.3) -> ar(sig, attack, release)
```

Default values must be numeric literals. Required parameters must precede optional ones (same as Python).

### 3.7 Mini-Notation Literals

```ebnf
mini_literal = pattern_kw "(" string [ "," closure ] ")" ;
pattern_kw   = "pat" | "seq" | "timeline" | "note" ;
```

The string contains mini-notation (see Section 5). The optional closure receives event data.

Examples:
```
pat("bd sd bd sd")
seq("c4 e4 g4", (t, v, p) -> saw(p) * v)
```

## 4. Pipe Semantics

### 4.1 Pipe as Let-Binding Rewrite

The pipe operator `|>` is a syntactic rewrite. It evaluates the left-hand side once and substitutes all `%` holes in the right-hand side with that value:

```
LHS |> RHS    →    let $temp = LHS in RHS[% → $temp]
```

### 4.2 Rewrite Examples

| Expression | Rewrite |
|------------|---------|
| `a \|> f(%)` | `f(a)` |
| `a \|> f(%, %)` | `let x = a in f(x, x)` |
| `a \|> f(%) \|> g(%)` | `let x = a in let y = f(x) in g(y)` |
| `a \|> % + % * 0.5` | `let x = a in x + x * 0.5` |
| `foo(a \|> f(%))` | `foo(f(a))` |
| `(x) -> x \|> f(%)` | `(x) -> f(x)` |

### 4.3 The Hole (`%`)

The `%` symbol references the left-hand side of the enclosing pipe.

```
saw(440) |> lp(%, 1000)     // % is the saw output
         |> delay(%, 0.5)   // % is the filtered output
         |> % * 0.5         // % is the delayed output
```

**Multiple Holes:** All `%` in a pipe stage receive the **same** value (evaluated once).

```
saw(440) |> lp(%, sin(%))   // both % are the same saw output
```

### 4.4 Pipes in Arguments and Closures

Pipes can appear anywhere an expression is valid:

```
// Pipe as function argument
reverb(saw(440) |> lp(%, 1000))

// Pipe in closure body (closure is greedy)
p.map(hz -> saw(hz) |> lp(%, 1000))

// Pipe the closure itself (needs parens)
((x) -> x * 2) |> apply(%, 42)
```

## 5. Operator Desugaring

The parser produces an AST where all binary operators become function calls:

| Expression | AST Representation |
|------------|-------------------|
| `a + b` | `add(a, b)` |
| `a - b` | `sub(a, b)` |
| `a * b` | `mul(a, b)` |
| `a / b` | `div(a, b)` |
| `a ^ b` | `pow(a, b)` |

**Negation:** There is no unary minus operator. Use these patterns:
- `x * -1` — lexer produces `-1` as a single negative number literal
- `x * neg(y)` — explicit negation function
- `neg(x)` desugars to `sub(0, x)` in semantic analysis

## 6. Mini-Notation Grammar

Mini-notation appears inside pattern strings and has its own sub-grammar.

### 6.1 Structure

```ebnf
mini_content = { mini_element } ;
mini_element = mini_atom [ modifier ] ;
```

### 6.2 Atoms

```ebnf
mini_atom = pitch_token
          | chord_token
          | inline_chord
          | sample_token
          | rest
          | group
          | sequence
          | polyrhythm
          | "(" mini_content ")" ;

pitch_token  = pitch_name [ octave ] ;
chord_token  = pitch_token ":" chord_type ;
inline_chord = pitch_token pitch_token { pitch_token } ;
sample_token = letter { letter | digit | "_" } [ ":" digit ] ;
rest         = "~" | "_" ;
```

**Pitch tokens** inside mini-notation: octave is **optional** (defaults to 4).

Examples: `c`, `c4`, `f#`, `Bb3`

### 6.3 Groupings

```ebnf
group      = "[" mini_content "]" ;
sequence   = "<" mini_content ">" ;
polyrhythm = "[" mini_atom { "," mini_atom } "]" ;
```

- `[a b c]` — subdivide: all events in one cycle
- `<a b c>` — sequence: one event per cycle, rotating
- `[a, b, c]` — polyrhythm: all events play simultaneously

### 6.4 Modifiers

```ebnf
modifier = speed_mod | length_mod | weight_mod | repeat_mod | chance_mod ;

speed_mod  = ( "*" | "/" ) number ;
length_mod = ":" number ;
weight_mod = "@" number ;
repeat_mod = "!" [ number ] ;
chance_mod = "?" [ number ] ;
```

| Modifier | Meaning | Example |
|----------|---------|---------|
| `*n` | Speed up by n | `c4*2` |
| `/n` | Slow down by n | `c4/2` |
| `:n` | Duration of n steps | `c4:4` |
| `@n` | Weight/probability | `c4@0.5` |
| `!n` | Repeat n times | `c4!3` |
| `?n` | Chance (0-1) | `c4?0.5` |

### 6.5 Euclidean Rhythms

```ebnf
euclidean = mini_atom "(" number "," number [ "," number ] ")" ;
```

`x(k,n)` — k hits over n steps
`x(k,n,r)` — with rotation r

Example: `bd(3,8)` — 3 kicks over 8 steps

### 6.6 Choice

```ebnf
choice = mini_atom { "|" mini_atom } ;
```

Random selection each cycle: `bd | sd | hh`

## 7. Clock System

### 7.1 Timing

- **BPM:** Beats per minute (set via `bpm = 120`)
- **Cycle:** 1 cycle = 4 beats by default
- **Cycle Duration:** `T = (60 / BPM) * 4` seconds

### 7.2 Built-in Timing Signals

| Identifier | Description |
|------------|-------------|
| `co` | Cycle offset: 0→1 ramp over one cycle |
| `beat(n)` | Phasor completing every n beats |

## 8. Chord Expansion

Chord literals and inline chords expand to frequency arrays:

```
'c4:maj'   -> [261.6, 329.6, 392.0]  // C, E, G in Hz
'c3e3g3'   -> [130.8, 164.8, 196.0]  // inline chord
```

When passed to a UGen expecting a scalar:
1. The UGen is duplicated for each frequency
2. Outputs are summed by default
3. Use `.map()` for custom per-voice processing:
   ```
   p.map(hz -> saw(hz) |> lp(%, 1000))
   ```

## 9. Complete Example

```
bpm = 120

pad = seq("c3e3g3b3:4 g3b3d4:4 a3c4e4:4 f3a3c4:4", (t, v, p) -> {
    env = ar(attack: 0.5, release: 2, trig: t)
    p.map(hz -> saw(hz)) * env * v * 0.1
})
|> svflp(in: %, cut: 400 + 300 * co, q: 0.7)
|> delay(in: %, time: 0.375, fb: 0.4) * 0.5 + %
|> out(L: %, R: %)
```

## 10. Grammar Summary (Complete EBNF)

```ebnf
(* Program *)
program     = { statement } ;
statement   = assignment | post_stmt | pipe_expr ;
assignment  = identifier "=" pipe_expr ;
post_stmt   = "post" "(" closure ")" ;

(* Expressions - lowest to highest precedence *)
pipe_expr   = add_expr { "|>" add_expr } ;
add_expr    = mul_expr { ( "+" | "-" ) mul_expr } ;
mul_expr    = pow_expr { ( "*" | "/" ) pow_expr } ;
pow_expr    = method_expr { "^" method_expr } ;
method_expr = primary { "." identifier "(" [ arg_list ] ")" } ;
primary     = atom | "(" pipe_expr ")" ;

(* Atoms *)
atom = number | bool_literal | string | pitch_literal | chord_literal
     | identifier | hole | function_call | closure | mini_literal ;
hole = "%" ;

(* Functions and Methods *)
function_call = identifier "(" [ arg_list ] ")" ;
arg_list      = argument { "," argument } ;
argument      = [ identifier ":" ] pipe_expr ;

(* Closures *)
closure      = "(" [ param_list ] ")" "->" closure_body ;
param_list   = identifier { "," identifier } ;
closure_body = block | pipe_expr ;
block        = "{" { statement } [ pipe_expr ] "}" ;

(* Patterns *)
mini_literal = pattern_kw "(" string [ "," closure ] ")" ;
pattern_kw   = "pat" | "seq" | "timeline" | "note" ;

(* Lexical *)
identifier   = letter { letter | digit | "_" } ;
number       = [ "-" ] digit { digit } [ "." digit { digit } ] ;
string       = quote { character } quote ;
quote        = '"' | "'" | "`" ;
letter       = "a"..."z" | "A"..."Z" | "_" ;
digit        = "0"..."9" ;
```

## 11. Compiler Implementation Notes

This section provides guidance for implementing the Akkado compiler. See `docs/initial_prd.md` for the full technical specification.

### 11.1 Lexer: String Interning

Use **string interning** to convert identifiers and keywords into unique `uint32_t` IDs. This allows the parser to perform integer comparisons instead of string comparisons.

```
"saw" → intern("saw") → 42
"saw" → intern("saw") → 42  (same ID)
```

Use **FNV-1a** hashing for fast, non-cryptographic identifier hashing.

### 11.2 Parser: Data-Oriented AST

Store the AST in a **contiguous arena** (`std::vector<Node>`) rather than heap-allocating individual nodes:

- Use `uint32_t` indices for child/sibling links instead of pointers
- Improves cache locality and reduces memory overhead
- Enables simple serialization

```cpp
struct Node {
    NodeType type;
    uint32_t first_child;   // Index into arena
    uint32_t next_sibling;  // Index into arena
    uint32_t token_id;      // Interned identifier
    // ... payload
};
```

### 11.3 Semantic ID Path Tracking (Hot-Swap)

For live-coding state preservation, maintain a **path stack** during AST construction. Each node receives a stable **semantic ID** derived from its path:

```
main/track1/osc → FNV-1a hash → 0x7A3B2C1D
```

When code is updated:
1. Compare semantic IDs between old and new DAG
2. Re-bind matching IDs to existing state in the StatePool
3. Apply micro-crossfade (5-10ms) for structural changes

### 11.4 DAG Construction

After parsing, flatten the AST into a **Directed Acyclic Graph** representing signal flow:

1. **Topological sort** (Kahn's algorithm or DFS) to determine execution order
2. All buffer dependencies must be satisfied before a node executes
3. Result: linear array of bytecode instructions

### 11.5 Bytecode Format

Each instruction is **128 bits (16 bytes)** for fast decoding:

```
[opcode:8][rate:8][out:16][in0:16][in1:16][in2:16][reserved:16][state_id:32]
```

See `cedar/include/cedar/vm/instruction.hpp` for the current implementation.

### 11.6 Threading Model

- **Triple buffer**: Compiler writes to "Next", audio thread reads from "Current"
- **Atomic pointer swap** at block boundaries
- **Lock-free SPSC queues** for parameter updates
