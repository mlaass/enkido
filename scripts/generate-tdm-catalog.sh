#!/bin/bash
set -euo pipefail

# generate-tdm-catalog.sh — build the Tidal Drum Machines catalog.
#
# Clones geikha/tidal-drum-machines (which ships no manifest), walks its
# machines/<Machine>/<machine-drumtype>/*.wav layout, and emits a single
# combined catalog JSON pinned to the upstream commit SHA. The catalog is
# text (~0.5-1 MB) and is committed to NKIDO; the audio is never bundled.
#
# Run manually whenever the catalog should be refreshed. Idempotent.
#
# Usage: scripts/generate-tdm-catalog.sh

cd "$(dirname "$0")/.."
REPO_ROOT="$(pwd)"

UPSTREAM="https://github.com/geikha/tidal-drum-machines"
OUT="$REPO_ROOT/web/static/samples/tidal-drum-machines/catalog.json"

# --- Dependencies ---
for cmd in git python3; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Error: required command '$cmd' not found" >&2
        exit 1
    fi
done

# --- Clone upstream into a temp dir ---
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "Cloning $UPSTREAM ..."
git clone --depth 1 --quiet "$UPSTREAM" "$TMP/repo"

COMMIT="$(git -C "$TMP/repo" rev-parse HEAD)"
echo "Upstream commit: $COMMIT"

if [ ! -d "$TMP/repo/machines" ]; then
    echo "Error: upstream layout changed — no machines/ directory" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

# --- Walk the repo and emit the catalog ---
# The directory walk + JSON serialization is done in Python so filenames
# with spaces and mixed case ('.WAV'/'.wav') are handled correctly.
REPO="$TMP/repo" COMMIT="$COMMIT" OUT="$OUT" python3 - <<'PY'
import json, os, sys, datetime

repo    = os.environ["REPO"]
commit  = os.environ["COMMIT"]
out     = os.environ["OUT"]
machines_dir = os.path.join(repo, "machines")

base = (
    "https://raw.githubusercontent.com/geikha/tidal-drum-machines/"
    f"{commit}/machines/"
)

banks = {}
n_sounds = 0
n_files = 0

for machine in sorted(os.listdir(machines_dir)):
    mpath = os.path.join(machines_dir, machine)
    if not os.path.isdir(mpath):
        continue

    prefix = machine.lower() + "-"
    sounds = {}
    for sub in sorted(os.listdir(mpath)):
        spath = os.path.join(mpath, sub)
        if not os.path.isdir(spath):
            continue

        # Sound code = sub-folder name with the leading "<machine>-" prefix
        # stripped; fall back to the full sub-folder name when it does not
        # match the expected pattern. A few upstream folders double the
        # separator ("oberheimdmx--perc") — drop any leftover leading dash.
        code = sub[len(prefix):] if sub.lower().startswith(prefix) else sub
        code = code.lstrip("-") or code

        wavs = sorted(
            f for f in os.listdir(spath)
            if f.lower().endswith(".wav") and os.path.isfile(os.path.join(spath, f))
        )
        if not wavs:
            continue  # empty sound sub-folder — omit

        sounds[code] = [f"{sub}/{w}" for w in wavs]
        n_sounds += 1
        n_files += len(wavs)

    if not sounds:
        continue  # machine with no usable sounds — omit

    bank = {"_path": f"{machine}/"}
    bank.update(sounds)
    banks[machine] = bank

catalog = {
    "_source": "github:geikha/tidal-drum-machines",
    "_commit": commit,
    "_generated": datetime.date.today().isoformat(),
    "_base": base,
    "banks": banks,
}

with open(out, "w") as fh:
    json.dump(catalog, fh, indent=1, sort_keys=False)
    fh.write("\n")

print(f"Machines: {len(banks)}  Sounds: {n_sounds}  Files: {n_files}")
print(f"Wrote {out}")
PY

echo "Done."
