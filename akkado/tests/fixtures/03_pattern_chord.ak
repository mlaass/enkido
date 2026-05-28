// Chord-mode pattern. Exercises F3's chord re-parse path
// (pattern_eval.cpp:206 + codegen_patterns.cpp re-parse sites).
c"CM Am F G"
    |> poly(@, ({freq}) -> sine(freq))
    |> out(@, @)
