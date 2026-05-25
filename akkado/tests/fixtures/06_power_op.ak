// Right-associative '^' operator. Bytecode changes intentionally in
// Phase 2 when 2^3^2 starts evaluating to 512 instead of 64; the
// const-folded value lands in a PUSH_CONST visible in this snapshot.
osc("sin", 2^3^2 * 100) |> out(@)
