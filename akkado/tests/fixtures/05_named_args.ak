// Spread record + named arg. Exercises Phase 1a's
// reorder_spread_named_args path (codegen.cpp:3233 — in-place arena
// rewrite to be replaced by a local ArgInfo vector).
fn f(a, b) -> a + b
r = {a: 1}
f(..r, b: 99) |> out(@)
