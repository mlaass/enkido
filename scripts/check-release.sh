#!/usr/bin/env bash
# scripts/check-release.sh
#
# Pre-release gate (docs/prd-memory-integrity-tests.md §3.6): runs every
# memory-integrity leg at deep iteration counts plus the sanitizer suite.
# Exits non-zero if any gate fails. The release ritual is:
#
#   ./scripts/check-release.sh && ./scripts/bump-version.sh <bump>
#
# bump-version.sh stays focused on version mechanics; this script owns the
# pre-release verification.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

export LSAN_OPTIONS="suppressions=$REPO_ROOT/cedar/tests/lsan.supp"
export ASAN_OPTIONS="detect_leaks=1:abort_on_error=0"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

echo "== Leg 2: Sanitizer build (ASan + LSan + UBSan) =="
cmake --preset sanitize >/dev/null
cmake --build build/sanitize --target cedar_tests akkado_tests -j
./build/sanitize/cedar/tests/cedar_tests
./build/sanitize/akkado/tests/akkado_tests

echo
echo "== Release build (tools + tests) =="
cmake --preset release -DNKIDO_BUILD_TESTS=ON >/dev/null
cmake --build build/release --target akkado_cli nkido_cli cedar_tests akkado_tests -j
export NKIDO_BIN_DIR="$REPO_ROOT/build/release/bin"

echo
echo "== Leg 1: Explosion guard =="
scripts/memory/check_corpus.sh

echo
echo "== Leg 3: Zero-allocation trap =="
./build/release/cedar/tests/cedar_tests "[zero_alloc]"

echo
echo "== Leg 4: Drift fuzz (100k iters under the RSS guard) =="
python3 scripts/memory/run_with_limit.py \
    --binary build/release/akkado/tests/akkado_tests \
    --rss-mb 1024 --timeout-sec 7200 \
    -- "[drift_fuzz]" --iters 100000

echo
echo "check-release: ALL GATES PASS"
