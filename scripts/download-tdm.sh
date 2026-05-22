#!/bin/bash
set -euo pipefail

# download-tdm.sh — populate the local Tidal Drum Machines cache.
#
# Downloads the samples of selected drum machines (or all of them) into
#   ${XDG_CACHE_HOME:-$HOME/.cache}/nkido/tidal-drum-machines/<Machine>/...
# preserving the relative paths the catalog encodes. nkido-cli auto-discovers
# this cache and plays catalog kits offline; machines not downloaded still
# stream from GitHub when online.
#
# Usage:
#   scripts/download-tdm.sh RolandTR808 RolandTR909 LinnDrum
#   scripts/download-tdm.sh --all
#   scripts/download-tdm.sh --list      # print available machine names

cd "$(dirname "$0")/.."
REPO_ROOT="$(pwd)"

# --- Dependencies ---
for cmd in python3 curl; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Error: required command '$cmd' not found" >&2
        exit 1
    fi
done

# --- Locate catalog.json (walk up from CWD, like the CLI) ---
find_catalog() {
    if [ -n "${NKIDO_TDM_CATALOG:-}" ]; then
        echo "$NKIDO_TDM_CATALOG"
        return 0
    fi
    local dir="$REPO_ROOT"
    while [ -n "$dir" ]; do
        local cand="$dir/web/static/samples/tidal-drum-machines/catalog.json"
        if [ -f "$cand" ]; then
            echo "$cand"
            return 0
        fi
        local parent
        parent="$(dirname "$dir")"
        [ "$parent" = "$dir" ] && break
        dir="$parent"
    done
    return 1
}

CATALOG="$(find_catalog || true)"
if [ -z "$CATALOG" ] || [ ! -f "$CATALOG" ]; then
    echo "Error: catalog.json not found. Run scripts/generate-tdm-catalog.sh first." >&2
    exit 1
fi

# --- Argument handling ---
if [ $# -eq 0 ]; then
    echo "Usage: $0 <Machine> [Machine ...] | --all | --list" >&2
    exit 1
fi

CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/nkido/tidal-drum-machines"

# --list: print machine names and exit.
if [ "$1" = "--list" ]; then
    CATALOG="$CATALOG" python3 - <<'PY'
import json, os
c = json.load(open(os.environ["CATALOG"]))
for name in sorted(c["banks"]):
    bank = c["banks"][name]
    n = sum(len(v) for k, v in bank.items() if k != "_path")
    print(f"{name}\t{n} files")
PY
    exit 0
fi

# --- Resolve the work list: emit "<encoded_url>\t<local_path>" per file ---
# Python builds the percent-encoded GitHub URL and the decoded local path.
WORKLIST="$(CATALOG="$CATALOG" CACHE_DIR="$CACHE_DIR" python3 - "$@" <<'PY'
import json, os, sys, urllib.parse

catalog   = json.load(open(os.environ["CATALOG"]))
cache_dir = os.environ["CACHE_DIR"]
base      = catalog["_base"]
banks     = catalog["banks"]

args = sys.argv[1:]
if args == ["--all"]:
    requested = sorted(banks)
else:
    requested = args
    unknown = [m for m in requested if m not in banks]
    if unknown:
        sys.stderr.write("Error: unknown machine(s): " + ", ".join(unknown) + "\n")
        sys.stderr.write("Run with --list to see available machines.\n")
        sys.exit(2)

for machine in requested:
    bank = banks[machine]
    path = bank["_path"]
    for code, variants in bank.items():
        if code == "_path":
            continue
        for rel in variants:
            full_rel = path + rel  # e.g. RolandTR808/rolandtr808-bd/BD0000.WAV
            url = base + urllib.parse.quote(full_rel)
            local = os.path.join(cache_dir, full_rel)
            print(f"{url}\t{local}")
PY
)"

# --- Download, skipping files already present ---
total=0
fetched=0
skipped=0
failed=0
current_machine=""

while IFS=$'\t' read -r url local; do
    [ -z "$url" ] && continue
    total=$((total + 1))

    # Print a per-machine header (machine = first path segment under CACHE_DIR).
    machine="${local#"$CACHE_DIR"/}"
    machine="${machine%%/*}"
    if [ "$machine" != "$current_machine" ]; then
        current_machine="$machine"
        echo "  $machine"
    fi

    if [ -f "$local" ]; then
        skipped=$((skipped + 1))
        continue
    fi

    mkdir -p "$(dirname "$local")"
    if curl -fsSL "$url" -o "$local.part" 2>/dev/null; then
        mv "$local.part" "$local"
        fetched=$((fetched + 1))
    else
        rm -f "$local.part"
        failed=$((failed + 1))
        echo "    warning: failed to download $url" >&2
    fi
done <<< "$WORKLIST"

echo ""
echo "Done. $total files: $fetched downloaded, $skipped already cached, $failed failed."
echo "Cache: $CACHE_DIR"
[ "$failed" -gt 0 ] && exit 1
exit 0
