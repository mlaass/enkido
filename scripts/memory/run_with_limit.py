#!/usr/bin/env python3
"""RSS-ceiling + wall-clock wrapper around a single binary invocation.

Part of the memory-integrity harness (docs/prd-memory-integrity-tests.md
Leg 1). Sets ``NKIDO_RLIMIT_MB`` in the child environment (the in-process
``setrlimit(RLIMIT_AS)`` backstop), launches the binary, and polls its
peak RSS out of band. The process is killed and the wrapper exits non-zero
if either the RSS ceiling or the wall-clock timeout is exceeded.

Usage:
    run_with_limit.py --binary BIN --rss-mb N --timeout-sec T -- ARGS...

On success prints a single machine-parseable line:
    PEAK_RSS_MB=<n> WALL_MS=<n>

On failure prints a FAIL line describing which ceiling was hit and exits
non-zero. RSS is sampled every 100 ms from /proc/<pid>/status on Linux or
``ps`` on macOS.
"""

import argparse
import os
import shutil
import subprocess
import sys
import time

POLL_INTERVAL_SEC = 0.1


def read_rss_kb_linux(pid: int) -> int:
    """Peak-of-now RSS for *pid* in KB from /proc, or 0 if unavailable."""
    try:
        with open(f"/proc/{pid}/status", "r") as fh:
            for line in fh:
                if line.startswith("VmRSS:"):
                    # "VmRSS:    12345 kB"
                    return int(line.split()[1])
    except (FileNotFoundError, ProcessLookupError, ValueError, IndexError):
        return 0
    return 0


def read_rss_kb_ps(pid: int) -> int:
    """RSS for *pid* in KB via `ps` (macOS / non-Linux fallback)."""
    ps = shutil.which("ps")
    if not ps:
        return 0
    try:
        out = subprocess.check_output(
            [ps, "-o", "rss=", "-p", str(pid)],
            stderr=subprocess.DEVNULL,
        )
        text = out.decode().strip()
        return int(text) if text else 0
    except (subprocess.CalledProcessError, ValueError):
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="Path to the binary to run")
    parser.add_argument("--rss-mb", type=int, required=True, help="Peak RSS ceiling (MB)")
    parser.add_argument(
        "--timeout-sec", type=float, required=True, help="Wall-clock timeout (seconds)"
    )
    parser.add_argument(
        "--label", default=None, help="Optional label for reporting (default: binary name)"
    )
    parser.add_argument(
        "--ignore-exit-code",
        action="store_true",
        help=(
            "Treat a clean (non-signalled) non-zero exit as success. Used by the "
            "explosion guard: a corpus input that fails to COMPILE (exit 1) is not a "
            "memory failure. RSS/timeout breaches and signal kills (incl. the "
            "RLIMIT_AS bad_alloc/abort) still fail."
        ),
    )
    parser.add_argument("args", nargs=argparse.REMAINDER, help="-- ARGS... for the binary")
    ns = parser.parse_args()

    child_args = ns.args
    if child_args and child_args[0] == "--":
        child_args = child_args[1:]

    label = ns.label or os.path.basename(ns.binary)

    if not (os.path.isfile(ns.binary) and os.access(ns.binary, os.X_OK)):
        print(f"FAIL [{label}]: binary not found or not executable: {ns.binary}",
              file=sys.stderr)
        return 2

    rss_ceiling_kb = ns.rss_mb * 1024
    read_rss = read_rss_kb_linux if sys.platform.startswith("linux") else read_rss_kb_ps

    env = dict(os.environ)
    # Hand the in-process RLIMIT_AS backstop the same number, so the OS kills
    # the child cleanly even between our 100 ms polls.
    env["NKIDO_RLIMIT_MB"] = str(ns.rss_mb)

    start = time.monotonic()
    try:
        proc = subprocess.Popen([ns.binary, *child_args], env=env)
    except OSError as exc:
        print(f"FAIL [{label}]: could not launch: {exc}", file=sys.stderr)
        return 2

    peak_rss_kb = 0
    failure = None
    while True:
        ret = proc.poll()
        rss_kb = read_rss(proc.pid)
        if rss_kb > peak_rss_kb:
            peak_rss_kb = rss_kb

        if rss_kb > rss_ceiling_kb:
            failure = (
                f"RSS ceiling exceeded: {rss_kb // 1024} MB > {ns.rss_mb} MB"
            )
            proc.kill()
            proc.wait()
            break

        elapsed = time.monotonic() - start
        if elapsed > ns.timeout_sec:
            failure = f"wall-clock timeout exceeded: {elapsed:.1f}s > {ns.timeout_sec}s"
            proc.kill()
            proc.wait()
            break

        if ret is not None:
            break

        time.sleep(POLL_INTERVAL_SEC)

    wall_ms = int((time.monotonic() - start) * 1000)
    peak_rss_mb = peak_rss_kb // 1024

    if failure is not None:
        print(f"FAIL [{label}]: {failure} "
              f"(PEAK_RSS_MB={peak_rss_mb} WALL_MS={wall_ms})", file=sys.stderr)
        return 1

    returncode = proc.returncode
    if returncode != 0:
        # The OS may have killed it via RLIMIT_AS (SIGKILL/SIGABRT from an
        # uncaught bad_alloc) — surface that as a memory failure rather than a
        # generic non-zero exit. A clean non-zero exit is the binary's own
        # decision (e.g. a compile error); under --ignore-exit-code that is not
        # a memory failure.
        signalled = returncode < 0
        if signalled:
            print(f"FAIL [{label}]: killed by signal {abs(returncode)} "
                  f"(PEAK_RSS_MB={peak_rss_mb} WALL_MS={wall_ms})", file=sys.stderr)
            return 1
        if not ns.ignore_exit_code:
            print(f"FAIL [{label}]: non-zero exit {returncode} "
                  f"(PEAK_RSS_MB={peak_rss_mb} WALL_MS={wall_ms})", file=sys.stderr)
            return 1
        print(f"PEAK_RSS_MB={peak_rss_mb} WALL_MS={wall_ms} EXIT={returncode}")
        return 0

    print(f"PEAK_RSS_MB={peak_rss_mb} WALL_MS={wall_ms}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
