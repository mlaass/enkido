#!/bin/bash
set -e

cd "$(dirname "$0")/.."

echo "=== Building with coverage ==="
cmake --preset coverage
cmake --build build/coverage

echo "=== Running cedar tests ==="
./build/coverage/cedar/tests/cedar_tests

echo "=== Running akkado tests ==="
./build/coverage/akkado/tests/akkado_tests

echo "=== Generating coverage report ==="
cd build/coverage

# Capture coverage data
lcov --capture \
    --directory . \
    --output-file coverage.info \
    --ignore-errors empty,gcov,mismatch \
    --gcov-tool gcov \
    --rc branch_coverage=1

# Remove external code (catch2, system headers, tests themselves)
lcov --remove coverage.info \
    '/usr/*' \
    '*/catch2/*' \
    '*/_deps/*' \
    '*/tests/*' \
    --output-file coverage_filtered.info \
    --ignore-errors empty,unused \
    --rc branch_coverage=1

# Generate HTML report
genhtml coverage_filtered.info \
    --output-directory coverage_report \
    --title "Enkido Test Coverage" \
    --legend \
    --branch-coverage \
    --ignore-errors source

echo ""
echo "=== Coverage Summary ==="
lcov --summary coverage_filtered.info --rc branch_coverage=1 2>&1 | grep -E "lines|functions|branches"

echo ""
echo "=== Per-directory coverage ==="
lcov --list coverage_filtered.info 2>/dev/null | grep -E "^(cedar|akkado|include).*\|" | head -40

echo ""
echo "HTML report generated at: build/coverage/coverage_report/index.html"
