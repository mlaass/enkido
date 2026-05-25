// Mini-notation note pattern. Exercises the per-literal sub-arena Phase
// 1b introduces — codegen should produce identical bytecode before and
// after.
n"c4 e4 g4 c5" as e
    |> osc("sin", e.freq)
    |> @ * e.vel
    |> out(@)
