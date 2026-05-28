// Right-associative `^` operator (verified Phase 0, commit b203e2e).
// `2^3^2` evaluates as `2^(3^2) = 512` — the bytecode below shows the
// outer POW reading the inner POW's result. Locked by this snapshot;
// any reordering or value change is a parser regression.
osc("sin", 2^3^2 * 100) |> out(@)
