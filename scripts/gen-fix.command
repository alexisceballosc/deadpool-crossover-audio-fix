#!/bin/bash
# Build the proxy fmodex.dll from src/ and install it into the game's Binaries.
# Works whether or not the game is present: the Binaries path is created if needed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src"
BIN="$ROOT/dist/Deadpool/Binaries"
OUT="$BIN/fmodex.dll"
OG="$BIN/fmodex_og.dll"

command -v python3 >/dev/null || { echo "error: python3 not found"; exit 1; }
command -v i686-w64-mingw32-gcc >/dev/null || { echo "error: i686-w64-mingw32-gcc not found (brew install mingw-w64)"; exit 1; }

mkdir -p "$BIN"

# First install only: the game ships its real FMOD as fmodex.dll. Move it aside to
# fmodex_og.dll (the name the proxy forwards to) before we take over fmodex.dll.
# Once fmodex_og.dll exists it is the real FMOD and is never touched again, so
# re-running only rebuilds the proxy.
if [ ! -f "$OG" ] && [ -f "$OUT" ]; then
    mv "$OUT" "$OG"
    echo "Kept stock FMOD as fmodex_og.dll"
fi

echo "Building proxy ..."
cd "$SRC"
python3 gen_def/gen_def.py
i686-w64-mingw32-gcc -O2 -Wall -shared -static-libgcc -o "$OUT" proxy.c proxy.def

echo "Installed: $OUT"
[ -f "$OG" ] || echo "note: no fmodex_og.dll present, the proxy has nothing to forward to until the game is installed"
