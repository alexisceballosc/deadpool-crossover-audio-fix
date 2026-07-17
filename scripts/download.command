#!/bin/bash
# Download the Deadpool game archive into dist/. Safe to re-run: skips if present.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"
ZIP="$DIST/Deadpool.zip"
URL="https://archive.org/download/Deadpool-LivBs/Deadpool.zip"

command -v curl >/dev/null || { echo "error: curl not found"; exit 1; }

mkdir -p "$DIST"

if [ -f "$ZIP" ]; then
    echo "Already downloaded: $ZIP"
    exit 0
fi

echo "Downloading Deadpool into $DIST ..."
# Download to a temp name and move on success, so an interrupted run never leaves
# a half file that looks complete.
curl -L --fail -o "$ZIP.part" "$URL"
mv "$ZIP.part" "$ZIP"
echo "Done: $ZIP"
