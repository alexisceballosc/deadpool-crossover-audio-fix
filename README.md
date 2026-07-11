# Deadpool (2013) - Audio Echo Fix under Wine/CrossOver

**Environment:** macOS (Apple Silicon, Rosetta 2) · CrossOver 26.2 · July 2026

## What it fixes

Under Wine/CrossOver on Mac, all in-game audio (footsteps, gunshots, dialogue, ambience)
plays with echoes/out-of-sync copies that don't exist on native Windows. This fix
eliminates them completely while preserving the game's original audio effects.

## How to apply

> [!IMPORTANT]  
> This fix works over the original game files of [Deadpool](https://archive.org/details/Deadpool-LivBs) game

1. Go to `<game>\Binaries\` (where `DP.exe` is).
2. Rename the original `fmodex.dll` to `fmodex_og.dll`.
3. Copy this package's `fmodex.dll` next to it.

*(Or just drag n drop both files in and choose overwrite)*

> [!WARNING]  
> Loads get noticeably longer (~2 min on startup, ~30 s per loading screen). This is the cost of the fix.

**Uninstall:** delete `fmodex.dll`, rename `fmodex_og.dll` back.

## How it works

The game loads its ~1868 sound effects as MPEG *compressed samples*
(`FMOD_CREATECOMPRESSEDSAMPLE`): FMOD decodes them on the fly each time a voice
plays, with a pool of 64 shared decoders. Under CPU translation (Rosetta), that
realtime decode (block seeking + codec state swapping between voices) can't keep
up in time and FMOD repeats already-decoded blocks → every sound leaves
out-of-sync copies of itself = the echoes.

The fix is a **proxy** `fmodex.dll`: it forwards all 698 exports to the real FMOD
(`fmodex_og.dll`) and intercepts only `System::createSound`, replacing the
`FMOD_CREATECOMPRESSEDSAMPLE (0x200)` flag with `FMOD_CREATESAMPLE (0x100)` → each
sound is decoded to PCM once at load time (a sequential path, no deadline, that
works fine). The realtime decode disappears, and with it, the echoes.
This is also what explains the slow loads.

## Build by yourself

Requires mingw-w64 (macOS via Homebrew: `brew install mingw-w64`).

```sh
cd src
python3 gen_def/gen_def.py   # reads gen_def/exports.txt, writes src/proxy.def
i686-w64-mingw32-gcc -O2 -Wall -shared -static-libgcc -o fmodex.dll proxy.c proxy.def
```

## Dead ends

Investigated and ruled out with evidence (don't repeat these):

| Hypothesis | Test | Verdict |
|---|---|---|
| Wine WASAPI output (cursor timing) | Force DSOUND / WINMM | ✗ DSOUND worse (loops), WINMM same |
| Speaker/surround mode misdetected | Log `getDriverCaps`/`setSpeakerMode` | ✗ Stereo correct throughout the chain |
| Wine audio backend (`winecoreaudio`) | Minimal WASAPI repro in the bottle | ✗ Clean playback; also the only backend on Mac |
| Underruns replaying old data | Repro with forced 120 ms stalls | ✗ Produces clicks/cuts, never echoes |
| Broken clocks (QPC/timeGetTime under Rosetta) | Measured them in the bottle | ✗ Accurate |
| Voice starvation → relaunches (too few playback slots) | Force 512 channels / 256 software voices | ✗ No change |
| Same sound retriggering | Dedup `playSound` by Sound* (150 ms soft, 1200 ms hard) | ✗ Echoes intact |
| Insufficient MPEG codec pool (shared decoders) | maxMPEGcodecs 64→256 | ✗ `FMOD_ERR_MEMORY`, dead audio (32-bit process) |
| Bug fixed in a newer FMOD | FMOD Ex 4.44.64 (2016) | ✗ Identical echoes - it's Rosetta timing, not an FMOD bug |
| Block all DSP processing | Blocked `ChannelGroup::addDSP` entirely | ✗ Kills intentional DSPs too (voice radio filter, environment reverb) - degrades audio, barely mitigates echo |

**The turning point:** capturing FMOD's output to WAV *before* it reached Wine
(via WAVWRITER) showed the echo was already in the mix. That ruled out Wine and
CoreAudio entirely and pointed straight at FMOD's realtime decode path. The fix
in "How it works" above.

## Known traps

- The game checks `getVersion` == 4.36.02; a different FMOD version
  **silently disables audio**. 4.44.64 also changes the ABI of
  `DSPConnection::setMix` (two floats → one), which would need a bridge in the proxy.
