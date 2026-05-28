// Sanity fixture: simple oscillator + output.
// Every later phase should keep this byte-identical.
osc("sin", 440) |> out(@)
