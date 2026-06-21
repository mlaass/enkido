#!/usr/bin/env bash
# scripts/memory/check_corpus.sh
#
# Memory-integrity Leg 1 (docs/prd-memory-integrity-tests.md §3.2b):
# iterate a fixture corpus and run `akkado --check` on each input under the
# RSS+timeout wrapper, aggregating into a single pass/fail line.
#
# The akkado CLI is exercised in `--check` mode (compile-only, no bytecode
# write) because the trigger incident was an `akkado --check` explosion.
#
# Env overrides:
#   NKIDO_BIN_DIR   Directory holding the akkado/nkido binaries.
#                   Default: first existing of build/release/bin, build/bin.
#   NKIDO_BUDGET_AKKADO_CLI_MB / NKIDO_TIMEOUT_AKKADO_SEC  (from budgets.sh)

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# shellcheck source=scripts/memory/budgets.sh
source "$SCRIPT_DIR/budgets.sh"

# Resolve the binary directory.
if [[ -n "${NKIDO_BIN_DIR:-}" ]]; then
    BIN_DIR="$NKIDO_BIN_DIR"
elif [[ -x "$REPO_ROOT/build/release/bin/akkado" ]]; then
    BIN_DIR="$REPO_ROOT/build/release/bin"
elif [[ -x "$REPO_ROOT/build/bin/akkado" ]]; then
    BIN_DIR="$REPO_ROOT/build/bin"
else
    echo "FAIL: no akkado binary found. Build first (cmake --preset release) or set NKIDO_BIN_DIR." >&2
    exit 2
fi

AKKADO_BIN="$BIN_DIR/akkado"
WRAPPER="$SCRIPT_DIR/run_with_limit.py"

if [[ ! -x "$AKKADO_BIN" ]]; then
    echo "FAIL: akkado binary not executable: $AKKADO_BIN" >&2
    exit 2
fi

# Build the corpus list (§11.3): stdlib, curated fuzz corpus, test fixtures,
# and a representative subset of experiments.
declare -a CORPUS=()
add_glob() {
    for f in $1; do
        [[ -f "$f" ]] && CORPUS+=("$f")
    done
}

add_glob "$REPO_ROOT/akkado/stdlib/*.ak"
add_glob "$REPO_ROOT/akkado/tests/fuzz/corpus/*.ak"
add_glob "$REPO_ROOT/akkado/tests/fixtures/*.ak"
add_glob "$REPO_ROOT/experiments/*.akk"

if [[ ${#CORPUS[@]} -eq 0 ]]; then
    echo "FAIL: corpus is empty (no inputs found)." >&2
    exit 2
fi

echo "== Explosion guard: $AKKADO_BIN --check over ${#CORPUS[@]} inputs =="
echo "   ceiling=${NKIDO_BUDGET_AKKADO_CLI_MB}MB timeout=${NKIDO_TIMEOUT_AKKADO_SEC}s"

fail_count=0
peak_overall=0
for input in "${CORPUS[@]}"; do
    rel="${input#"$REPO_ROOT"/}"
    out="$(python3 "$WRAPPER" \
        --binary "$AKKADO_BIN" \
        --rss-mb "$NKIDO_BUDGET_AKKADO_CLI_MB" \
        --timeout-sec "$NKIDO_TIMEOUT_AKKADO_SEC" \
        --label "$rel" \
        --ignore-exit-code \
        -- --check "$input" 2>&1)"
    status=$?
    if [[ $status -eq 0 ]]; then
        peak="$(echo "$out" | sed -n 's/.*PEAK_RSS_MB=\([0-9]*\).*/\1/p')"
        [[ -n "$peak" && "$peak" -gt "$peak_overall" ]] && peak_overall="$peak"
        printf '  ok   %-50s %s\n' "$rel" "$out"
    else
        fail_count=$((fail_count + 1))
        printf '  FAIL %-50s %s\n' "$rel" "$out"
    fi
done

echo "------------------------------------------------------------"
if [[ $fail_count -eq 0 ]]; then
    echo "PASS: ${#CORPUS[@]} inputs, peak RSS ${peak_overall}MB (ceiling ${NKIDO_BUDGET_AKKADO_CLI_MB}MB)"
    exit 0
else
    echo "FAIL: $fail_count/${#CORPUS[@]} inputs exceeded budget or errored"
    exit 1
fi
