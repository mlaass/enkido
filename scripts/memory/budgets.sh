# scripts/memory/budgets.sh
#
# Centralised RSS / drift budgets for the memory-integrity harness
# (docs/prd-memory-integrity-tests.md §6.3). Source this file from any
# wrapper or CI job so every leg reads the same numbers. Changing a budget
# means touching this one file with a rationale in the commit message —
# hard ceilings stay loud and are never silently raised.

# Per-binary RSS ceilings (megabytes).
export NKIDO_BUDGET_AKKADO_CLI_MB=1024   # 48x under the 48 GB trigger incident
export NKIDO_BUDGET_NKIDO_CLI_MB=2048
export NKIDO_BUDGET_TEST_BIN_MB=4096

# Drift-fuzz budgets (Leg 4).
export NKIDO_BUDGET_DRIFT_PEAK_MB=1024
export NKIDO_BUDGET_DRIFT_SLOPE_BYTES_PER_ITER=1024

# Wall-clock timeouts (seconds).
export NKIDO_TIMEOUT_AKKADO_SEC=60
export NKIDO_TIMEOUT_NKIDO_SEC=300
export NKIDO_TIMEOUT_TEST_BIN_SEC=600
