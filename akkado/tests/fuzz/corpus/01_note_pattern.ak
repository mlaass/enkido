// Curated drift-fuzz seed: top-level alternation note pattern into an osc.
n"c4 e4 g4 b4" |> sine(%.freq) |> out(% * 0.3, % * 0.3)
