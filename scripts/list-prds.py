#!/usr/bin/env python3
"""List PRDs grouped by normalized status.

Walks docs/prd-*.md, parses the first status header it finds in each file,
normalizes the raw status string into one of {not-started, in-progress,
complete, on-hold, unknown}, and prints a filtered listing plus a summary.

Read-only. Never modifies any PRD.

Usage:
    python scripts/list-prds.py                  # actionable subset
    python scripts/list-prds.py --status complete
    python scripts/list-prds.py --all            # every PRD, grouped
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PRD_DIR = REPO_ROOT / "docs"
PRD_GLOB = "prd-*.md"
HEADER_SCAN_LINES = 20

BUCKETS = ("not-started", "in-progress", "complete", "on-hold", "unknown")
DEFAULT_BUCKETS = ("not-started", "in-progress", "unknown")

STATUS_LINE_RE = re.compile(
    r"""^\s*
        (?:>\s*)?                       # optional blockquote marker
        \*\*\s*Status\s*[:*]+\s*\*?\*?  # **Status:** / **Status**: / **Status: ...**
        \s*(?P<rest>.+?)\s*$
    """,
    re.IGNORECASE | re.VERBOSE,
)

RAW_STOP_RE = re.compile(r"(\*\*| — |\. |, |\() ")  # stops the raw keyword extraction


@dataclass
class PrdEntry:
    path: Path
    bucket: str
    raw: str | None  # original status text, None if no header parsed
    mtime: float  # file modification time (epoch seconds)


def format_mtime(mtime: float, now: datetime | None = None) -> str:
    """Format an mtime like `ls -lt`: 'Mon DD HH:MM' if current year, else 'Mon DD  YYYY'."""
    dt = datetime.fromtimestamp(mtime)
    now = now or datetime.now()
    if dt.year == now.year:
        return dt.strftime("%b %d %H:%M")
    return dt.strftime("%b %d  %Y")


def extract_raw_status(lines: list[str]) -> str | None:
    """Return the raw status keyword from the first matching header line, or None."""
    for line in lines[:HEADER_SCAN_LINES]:
        m = STATUS_LINE_RE.match(line)
        if not m:
            continue
        rest = m.group("rest").strip()
        # Strip trailing closing `**` if the whole `**Status: X**` was bolded together.
        rest = rest.rstrip("*").rstrip()
        # Cut at the first delimiter that ends the status keyword.
        stop = RAW_STOP_RE.search(rest + " ")
        if stop:
            rest = rest[: stop.start()].rstrip()
        return rest or None
    return None


def normalize(raw: str | None) -> str:
    """Map a raw status string to one of the canonical buckets."""
    if raw is None:
        return "unknown"
    u = re.sub(r"\s+", " ", raw.upper()).strip()

    # Order matters: on-hold and complete take precedence over partial matches.
    if "ON HOLD" in u or "DESCOPED" in u or "NOT CURRENTLY IMPLEMENTED" in u:
        return "on-hold"

    # in-progress markers that should beat the generic "DONE/COMPLETE" check.
    # e.g. "PHASE 1 COMPLETE", "Phase A in flight", "PARTIAL", "MOSTLY SHIPPED".
    if "MOSTLY SHIPPED" in u or u == "PARTIAL" or "INCONCLUSIVE" in u:
        return "in-progress"
    if "IN FLIGHT" in u:
        return "in-progress"
    if re.search(r"\bPHASE\b", u) and "ALL PHASES" not in u and (
        "COMPLETE" in u or "DONE" in u or "SHIPPED" in u or "IMPLEMENTED" in u
    ):
        return "in-progress"
    if "REVISION" in u and "IMPLEMENTED" in u:
        return "in-progress"

    if (
        "ALL PHASES DONE" in u
        or u.startswith("FULLY IMPLEMENTED")
        or "DONE" in u
        or "COMPLETE" in u
        or "SHIPPED" in u
        or "IMPLEMENTED" in u
    ):
        return "complete"

    if (
        "NOT STARTED" in u
        or u == "TODO"
        or u.startswith("TODO ")
        or "DRAFT" in u  # covers DRAFT and FIRST DRAFT
        or u == "PROPOSED"
    ):
        return "not-started"

    return "unknown"


def scan() -> list[PrdEntry]:
    entries: list[PrdEntry] = []
    for path in sorted(PRD_DIR.glob(PRD_GLOB)):
        try:
            with path.open("r", encoding="utf-8", errors="replace") as fh:
                lines = [next(fh, "") for _ in range(HEADER_SCAN_LINES)]
            mtime = path.stat().st_mtime
        except OSError as e:
            print(f"warning: could not read {path}: {e}", file=sys.stderr)
            continue
        raw = extract_raw_status(lines)
        entries.append(PrdEntry(path=path, bucket=normalize(raw), raw=raw, mtime=mtime))
    return entries


def format_line(entry: PrdEntry, name_width: int, now: datetime) -> str:
    status_col = entry.bucket.upper().replace("-", "_").ljust(12)
    date_col = format_mtime(entry.mtime, now).ljust(12)
    name_col = entry.path.name.ljust(name_width)
    if entry.raw is None:
        raw_col = "<none>"
    else:
        snippet = entry.raw.replace("\n", " ")
        if len(snippet) > 60:
            snippet = snippet[:57] + "..."
        raw_col = f'"{snippet}"'
    return f"{status_col}  {date_col}  {name_col}  [raw: {raw_col}]"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="List PRDs by normalized status.",
        epilog="With no flags, prints not-started + in-progress + unknown (the actionable subset).",
    )
    parser.add_argument(
        "--status",
        choices=BUCKETS + ("all",),
        help="Show only this bucket (or 'all').",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Show every PRD grouped by bucket. Equivalent to --status all.",
    )
    args = parser.parse_args(argv)

    if not PRD_DIR.is_dir():
        print(f"error: PRD directory not found: {PRD_DIR}", file=sys.stderr)
        return 2

    entries = scan()
    if not entries:
        print(f"no PRDs found under {PRD_DIR}/{PRD_GLOB}", file=sys.stderr)
        return 0

    if args.all or args.status == "all":
        selected = set(BUCKETS)
    elif args.status:
        selected = {args.status}
    else:
        selected = set(DEFAULT_BUCKETS)

    visible = [e for e in entries if e.bucket in selected]
    # Latest mtime first, like `ls -lt`. Tiebreak on filename for determinism.
    visible.sort(key=lambda e: (-e.mtime, e.path.name))
    name_width = max((len(e.path.name) for e in visible), default=0)
    now = datetime.now()

    for entry in visible:
        print(format_line(entry, name_width, now))

    counts = {b: sum(1 for e in entries if e.bucket == b) for b in BUCKETS}
    total = len(entries)
    width = max(len(str(v)) for v in list(counts.values()) + [total])
    print("---")
    for b in BUCKETS:
        print(f"{b:12} {counts[b]:>{width}}")
    print(f"{'total':12} {total:>{width}}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
