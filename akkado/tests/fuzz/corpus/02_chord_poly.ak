// Curated drift-fuzz seed: chord pattern through poly() into a mono synth.
fn lead({freq, gate, vel}) -> sine(freq) * vel * 0.05
c"<Am Em F G>" |> poly(@, lead, 4) |> out(@)
