#!/bin/bash
# Unzip the downloaded game archive into dist/Deadpool/. Safe to re-run.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"
ZIP="$DIST/Deadpool.zip"
GAME="$DIST/Deadpool"          # the archive's own top-level folder name

command -v unzip >/dev/null || { echo "error: unzip not found"; exit 1; }
[ -f "$ZIP" ] || { echo "error: no archive at $ZIP, run download.command first"; exit 1; }

if [ -f "$GAME/Binaries/DP.exe" ]; then
    echo "Already installed: $GAME"
    exit 0
fi

echo "Unzipping into $DIST ..."
unzip -q -o "$ZIP" -d "$DIST"

[ -f "$GAME/Binaries/DP.exe" ] || { echo "error: DP.exe not found after unzip, archive layout changed"; exit 1; }
echo "Done: $GAME"
