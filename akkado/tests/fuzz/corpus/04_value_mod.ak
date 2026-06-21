// Curated drift-fuzz seed: value pattern modulating amplitude.
v"<0.2 0.5 0.8 0.5>" * sine(330) |> reverb(@) |> out(@, @)
