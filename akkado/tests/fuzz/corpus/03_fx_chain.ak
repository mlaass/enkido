// Curated drift-fuzz seed: oscillator into filter + delay FX chain.
saw(220) |> lp(@, 1200) |> delay(@, 0.25, 0.4) |> out(@ * 0.3, @ * 0.3)
