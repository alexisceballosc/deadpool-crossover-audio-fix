# Deadpool (2013): Audio Fix for Wine/CrossOver

**Environment:** macOS (Apple Silicon, Rosetta 2) · CrossOver 26.2

## What it fixes

Under Wine/CrossOver on Mac, Deadpool's audio breaks in ways it does not on native Windows. Two separate bugs:

**1. Every sound trails copies of itself.** Footsteps, gunshots, dialogue, ambience, all of it echoing.

**2. Bursts of noise, saturation and reverb-like smearing,** arriving together and worse the busier the scene gets.

Both are fixed, the game keeps all of its sound effects, and it no longer stalls to a few frames per second while loading (see [The cache](#the-cache)).

> [!IMPORTANT]
> Works over the original game files of [Deadpool](https://archive.org/details/Deadpool-LivBs).

## How to apply

1. Go to `<game>\Binaries\` (where `DP.exe` is).
2. Rename the original `fmodex.dll` to `fmodex_og.dll`.
3. Copy this package's `fmodex.dll` next to it.

*(Or just drag both files in and choose overwrite.)*

**Uninstall:** delete `fmodex.dll`, rename `fmodex_og.dll` back.

## How it works

A proxy `fmodex.dll` sits between the game and the real FMOD (`fmodex_og.dll`): it forwards 693 of the 698 exports untouched and intercepts five. Both bugs are FMOD's, and each fix lives in one of those five.

**Bug 1, the echoes.** FMOD decoded the compressed audio in realtime, on every play. Under Rosetta that decode misses its deadline, so FMOD reuses blocks it already decoded and every sound trails copies of itself. The proxy has each sound decode to PCM once, at load, so there is no realtime decode left to fall behind.

**Bug 2, the noise.** The mixer was running at 113% of realtime, unable to produce audio as fast as it was consumed, so it dropped and repeated blocks. Two thirds of that load was seven echo, reverb and flange effects, five of them left connected while turned down to silence, because FMOD charges full price for an effect no matter how quiet it is. The proxy removes any effect that has gone silent and restores it the instant the game makes it audible. Every effect the game uses still plays.

## The cache

Decoding at load is the slow part under Rosetta, and left alone it would be paid on every launch. Instead the proxy writes each decoded sound to `Binaries\audio_cache\` the first time you hear it, and reads it back from there on every launch after. The first visit to an area costs what it always did; from then on it is instant, this session and every future one. Nothing to run, the cache fills itself as you play.

> [!TIP]
> **Don't delete `audio_cache\`.** A cache grown across a full playthrough is a complete, pre-decoded copy of the game's audio, and the basis for a build that never decodes at all.

The proxy also keeps the raw compressed bank (`.src`) beside each entry. You never need these, but to decode the whole set at once instead of playing through it, run `tools/decode_cache.py` (needs ffmpeg, `brew install ffmpeg`):

```sh
python3 tools/decode_cache.py "<game>/Binaries/audio_cache"
```

## Build

Requires mingw-w64 (`brew install mingw-w64`).

```sh
cd src
python3 gen_def/gen_def.py
i686-w64-mingw32-gcc -O2 -Wall -shared -static-libgcc -o fmodex.dll proxy.c proxy.def
```

**32-bit (`i686`) is mandatory**: the game is a 32-bit process.

---

**[TECHNICAL.md](TECHNICAL.md)** covers the measurements behind both fixes, how the game drives FMOD, the dead ends already ruled out, and the traps to know before touching the code. Read it first: most of the obvious ideas have been tried and disproved with data.
