# **Akkado: A Rhythmic & DSP Language Specification**

Akkado is a domain-specific language (DSL) designed for live-coding musical patterns and modular synthesis. It combines the rhythmic power of **Strudel/Tidal Mini-Notation** with a functional **Directed Acyclic Graph (DAG)** approach to audio signal processing.

## **1\. Core Philosophy**

* **The Pattern is the Trigger:** Patterns (defined via mini-notation) act as control signals ($Triggers, Velocities, Pitches$).  
* **The Closure is the Voice:** Anonymous functions bridge the gap between control data and audio synthesis.  
* **The Pipe is the Edge:** The |\> operator defines the flow of data through the graph.  
* **The Hole is the Port:** The % symbol represents an explicit input port for signal injection, allowing for serial and parallel processing.

## **2\. Formal EBNF Grammar**

### **2.1 High-Level Structure**

program             \= { statement | comment } ;  
statement           \= assignment | post\_processing | expression ;  
assignment          \= identifier \= expression ;  
post\_processing     \= "post" "(" closure ")" ;  
comment             \= "//" { all\_characters } "\\n" ;

(\* Precedence: Pipes (Lowest) \-\> Math \-\> Atoms (Highest) \*)  
expression          \= math\_expr { "|\> " term } ;

### **2.2 Terms and Atoms**

term                \= function\_call   
                    | closure   
                    | mini\_literal   
                    | hole   
                    | identifier   
                    | "(" expression ")" ;

primary             \= term | literal ;  
hole                \= "%" ;

### **2.3 Signal Mathematics**

math\_expr           \= multiplication { ( "+" | "-" ) multiplication } ;  
multiplication      \= power { ( "\*" | "/" ) power } ;  
power               \= primary { "^" primary } ;

### **2.4 Functions and Closures**

closure             \= "(" \[ arg\_decl\_list \] ")" "-\>" ( block | expression ) ;  
arg\_decl\_list       \= identifier { "," identifier } ;  
block               \= "{" { statement } \[ expression \] "}" ;

function\_call       \= identifier "(" \[ named\_arg\_list \] ")" ;  
named\_arg\_list      \= named\_argument { "," named\_argument } ;  
named\_argument      \= \[ identifier ":" \] expression ;

### **2.5 Literals & Tokens**

literal             \= number | pitch\_literal | chord\_literal | bool\_literal | string\_literal ;

pitch\_literal       \= "'" ( "a"..."g" | "A"..."G" ) \[ "\#" | "b" \] digit "'" ;  
chord\_literal       \= "'" ( pitch\_token { pitch\_token } | pitch\_token ":" chord\_type ) "'" ;  
chord\_type          \= "maj" | "min" | "dom7" | "maj7" | "min7" | "dim" | "aug" | "sus2" | "sus4" ;

number              \= \[ "-" \] digit { digit } \[ "." { digit } \] ;  
bool\_literal        \= "true" | "false" ;  
string\_literal      \= '"' { any\_character } '"' ;  
identifier          \= letter { letter | digit | "\_" } ;

mini\_literal        \= pattern\_type "(" quote mini\_content quote \[ "," closure \] ")" ;  
pattern\_type        \= "pat" | "seq" | "timeline" | "note" ;  
quote               \= "'" | '"' | "\`" ;

### **2.6 Basic Characters**

letter              \= "a"..."z" | "A"..."Z" ;  
digit               \= "0"..."9" ;  
any\_character       \= ? all visible characters ? ;  
all\_characters      \= any\_character | " " | "\\t" ;

## **3\. Mini-Notation Sub-Grammar**

The mini\_content string describes the distribution of events over time.

mini\_content        \= { sequence\_element | " " | "\\n" } ;  
sequence\_element    \= mini\_node \[ modifier \] ;

mini\_node           \= atom   
                    | group          (\* \[a b c\] \*)  
                    | sequence       (\* \<a b c\> \- one element per cycle \*)  
                    | polyphony      (\* \[a, b, c\] \- parallel/chords \*)  
                    | choice         (\* a | b | c \- random selection \*)  
                    | "(" mini\_content ")" ;

atom                \= pitch\_token | chord\_token | inline\_chord | sample\_token | rest\_token | euclidean ;  
group               \= "\[" mini\_content "\]" ;  
sequence            \= "\<" mini\_content "\>" ;  
polyphony           \= mini\_node { "," mini\_node } ;  
choice              \= mini\_node { "|" mini\_node } ;  
euclidean           \= mini\_node "(" number "," number \[ "," number \] ")" ;

modifier            \= speed\_mod | length\_mod | weight\_mod | repeat\_mod | chance\_mod ;  
speed\_mod           \= ( "\*" | "/" ) number ;  
length\_mod          \= ":" number ;           (\* e.g. c3:4 for 4-step duration \*)  
weight\_mod          \= "@" number ;  
repeat\_mod          \= "\!" \[ number \] ;  
chance\_mod          \= "?" \[ number \] ;

pitch\_token         \= ( "a"..."g" | "A"..."G" ) \[ "\#" | "b" \] \[ digit \] ;  
chord\_token         \= pitch\_token ":" chord\_type ;  
inline\_chord        \= pitch\_token { pitch\_token } ; (\* e.g. c3e3g3 \*)  
sample\_token        \= { letter | digit | "\_" | ":" } ;  
rest\_token          \= "\~" | "\_" ;

## **4\. Chord Support & Signal Expansion**

Flux-DAG treats chords as **Signal Arrays**. When a chord literal or a polyphonic pattern is passed to a UGen, the engine performs "Implicit Expansion."

### **4.1 Chord Definition Styles**

1. **Named Chords:** 'c4:maj' $\\rightarrow$ $\[261.6, 329.6, 392.0\]$ (Hz)  
2. **Inline Chords:** 'c3e3g3' $\\rightarrow$ $\[130.8, 164.8, 196.0\]$ (Hz)

### **4.2 Expansion Rules**

When a UGen receives an array of values (a chord) where it expects a single value:

1. **Oscillators:** The oscillator node is duplicated for each frequency in the array. The outputs are summed by default.  
2. **Manual Mapping:** Users can use .map() on a chord signal to define custom per-voice behavior.  
   * p.map(hz \-\> saw(hz) |\> lp(%, 1000))

## **5\. The Clock System**

### **5.1 BPM and Cycles**

* **BPM (Beats Per Minute):** Defines the pulse. By default, **1 Cycle \= 4 Beats**.  
* **Cycle Duration (**$T\_c$**):** $T\_c \= (60 / \\text{BPM}) \\times 4$.

### **5.2 Reserved Keywords**

* co: **Cycle Offset** (0 to 1 ramp over 1 cycle).  
* beat(n): Phasor completing every n beats.

## **6\. The Hole (%) Resolution Rules**

1. **Explicit Injection:** LHS is bound to every % on RHS.  
2. **Implicit Injection:** If no % exists, LHS is the first argument of the RHS function.  
3. **Variable Reuse:** Assignments create reusable nodes.

## **7\. Full Implementation Example**

bpm \= 120

// Using inline chords and length shorthand (:4) in a sequence  
pad \= seq('c3e3g3b3:4@1 c3e3g3b3d4:4@1 g3a\#3d4g4:4@1 e3g3b3e4:4@1', (trig, v, p) \-\> {  
  env \= ar(attack: 1, release: 3, trig: trig)  
  p.map(hz \-\> saw(hz)) \* env \* v \* 0.05  
})  
|\> svflp(in: %, cut: 400 \+ 300 \* sin(hz: 1/16 \* co), q: 0.7)  
|\> velvet(in: %, size: 2.8, damping: 0.6, decay: 0.9) \* 0.8 \+ %  
|\> lexicon(in: %, size: 3.5, diffusion: 0.7, damping: 0.6, modulation: 0.5) \* 0.6 \+ %  
|\> tanh(% \* 0.6)  
|\> out(L: %, R: delay(in: %, time: 0.002))  
