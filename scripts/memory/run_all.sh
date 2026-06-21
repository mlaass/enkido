#!/usr/bin/env bash
# scripts/memory/run_all.sh
#
# Local entry point for the memory-integrity harness
# (docs/prd-memory-integrity-tests.md §3.1). Builds the release tree and runs
# the fast tier of every leg that currently exists. Each leg is appended as
# its phase lands; today it builds the CLI tools + cedar_tests and runs:
#
#   Leg 1  Explosion guard       scripts/memory/check_corpus.sh
#   Leg 3  Zero-alloc trap       cedar_tests "[zero_alloc]"
#
# Target: exit 0 on a clean checkout in well under a minute on a warm build
# (PRD §11.2.1).
#
# Env overrides:
#   NKIDO_SKIP_BUILD=1   Reuse the existing build/release tree as-is.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build/release"

# ---------------------------------------------------------------------------
# Build (release + tests in one tree). The `release` preset sets
# NKIDO_BUILD_TESTS=OFF; we override so the same tree carries the CLI tools
# AND the Catch2 binaries the zero-alloc / drift legs will need in later
# phases.
# ---------------------------------------------------------------------------
if [[ "${NKIDO_SKIP_BUILD:-0}" != "1" ]]; then
    echo "== Build (release) =="
    cmake --preset release -DNKIDO_BUILD_TESTS=ON >/dev/null
    cmake --build "$BUILD_DIR" --target akkado_cli nkido_cli cedar_tests -j
fi

export NKIDO_BIN_DIR="$REPO_ROOT/$BUILD_DIR/bin"

overall=0

# ---------------------------------------------------------------------------
# Leg 1 — Explosion guard
# ---------------------------------------------------------------------------
echo
echo "== Leg 1: Explosion guard =="
if "$SCRIPT_DIR/check_corpus.sh"; then
    echo "Leg 1: PASS"
else
    echo "Leg 1: FAIL"
    overall=1
fi

# ---------------------------------------------------------------------------
# Leg 3 — Zero-allocation trap (cedar_tests "[zero_alloc]")
# ---------------------------------------------------------------------------
echo
echo "== Leg 3: Zero-allocation trap =="
CEDAR_TESTS="$BUILD_DIR/cedar/tests/cedar_tests"
if [[ -x "$CEDAR_TESTS" ]]; then
    if "$CEDAR_TESTS" "[zero_alloc]"; then
        echo "Leg 3: PASS"
    else
        echo "Leg 3: FAIL"
        overall=1
    fi
else
    echo "Leg 3: SKIP (cedar_tests not built)"
fi

echo
if [[ $overall -eq 0 ]]; then
    echo "run_all: ALL LEGS PASS"
else
    echo "run_all: ONE OR MORE LEGS FAILED"
fi
exit $overall
