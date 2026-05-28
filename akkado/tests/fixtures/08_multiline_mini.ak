// Multi-line mini-notation string. Exercises F8's mini-lexer line
// tracking. Bytecode should be byte-identical before and after Phase 2's
// fix — the fix only changes diagnostic positions, not codegen output.
n"c4 d4
  e4 f4
  g4 a4 b4 c5" as e
    |> sine(e.freq)
    |> out(@)
