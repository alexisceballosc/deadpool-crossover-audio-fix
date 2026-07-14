# Technical notes

Two bugs made Deadpool's audio unusable under Rosetta: every sound echoed, and dense scenes dissolved into noise. Both are fixed. This documents what was measured to find the causes, and the approaches that looked promising but led nowhere, so neither has to be rediscovered.

**Environment:** macOS on Apple Silicon (Rosetta 2) · CrossOver 26.2 · Unreal Engine 3 · **32-bit** process · FMOD Ex **4.36.02**

---

## Contents

1. [Bug 1: the echoes](#bug-1-the-echoes)
2. [Bug 2: the mixer cannot keep up](#bug-2-the-mixer-cannot-keep-up)
3. [How the game uses FMOD](#how-the-game-uses-fmod)
4. [Dead ends](#dead-ends)
5. [Rejected](#rejected)
6. [Constraints and traps](#constraints-and-traps)

---

## Bug 1: the echoes

The game loads 2189 sound effects as MPEG **compressed samples** (`FMOD_CREATECOMPRESSEDSAMPLE`), decoded on the fly every time a voice plays, from a pool of 64 shared decoders. Under Rosetta that realtime decode misses its deadline, so FMOD repeats blocks it already decoded and every sound trails out-of-sync copies of itself. The fix (`mode.h`) swaps the flag to `FMOD_CREATESAMPLE`, so each sound decodes to PCM once at load.

Capturing FMOD's output to WAV *before it reached Wine* (via WAVWRITER) shows the echo already present in the mix, which locates the fault inside FMOD and rules out Wine and CoreAudio.

### What decoding at load costs

| | |
|---|---|
| `createSound` on a sample, mean | **82.2 ms** |
| Peak | **1995 ms**, on an 8 KB sound |
| Correlation with sound size | **r = 0.929** (n = 2329) |
| Throughput under Rosetta | **~5.3 ms per compressed KB** |
| **Total per session** | **~191 seconds** |

The `r = 0.929` settles that this is real decode work scaling with data size, not a lock being waited on. During a preload `createSound` is inside FMOD ~100% of wall-clock time: that is the mid-level stutter, and it *is* the fix doing its work, not a bug in it. No tuning shrinks it while the work still happens.

### The cache removes the per-launch cost

The decode is otherwise paid on **every launch**. The proxy caches the decoded bank to `audio_cache\<hash>.pcm` and serves it next time, so the ~191 s is paid once ever: **191,507 ms → 24 ms** per session, measured over 2631 sounds, none rejected by FMOD.

Getting the PCM back out of FMOD means locking the container's **subsound**: `Sound::lock` returns `pcmbytes=0` on the FSB4 parent, because the parent holds no PCM. `tools/decode_cache.py` rebuilds the same banks offline from the dumped `.src` with native `ffmpeg`, **48x faster than in-game** (1915 banks in 4.0 s vs ~191 s), which is how a full cache gets built without replaying every area.

**A cache grown across a full playthrough is a complete, pre-decoded copy of the game's audio:** what a definitive build would ship with, so a fresh install pays nothing, even on a first visit.

---

## Bug 2: the mixer cannot keep up

Separate bug, separate cause; it survived the first fix. **One event, not three symptoms:** a burst of noise with reverberant smearing, worse the busier the scene. Over 100% mixer means it needs more than 1 s of CPU per 1 s of audio, misses its deadline, and FMOD drops and repeats blocks.

**Two thirds of the load was DSPs producing silence.** The game creates seven delay-line DSPs (4 `ECHO`, 2 `SFXREVERB`, 1 `FLANGE`), ramps five of them down to nothing, and **leaves them in the graph. FMOD does not skip a DSP because its wet mix is zero:** it processes every sample and charges full price. That was ~41% of the mixer, burned on silence. The fix bypasses any delay DSP whose wet mix (or reverb room) reaches zero, and restores it the moment the game gives it an audible mix.

| mixer CPU (`getCPUUsage`) | mean | p90 | max | over 100% |
|---|---|---|---|---|
| Before | **113.0%** | 155.5% | 227.5% | **62% of the time** |
| **After** | **94.5%** | 145.8% | 198.5% | **26%** |
| *(all delay DSPs out, for reference)* | *41.7%* | *45.4%* | *54.2%* | *0%* |

### The budget depends on the scene

```
mixing voices alone (no DSPs):   ~25% quiet  ->  54-67% dense
peak concurrent voices:          58 of the game's 64

DSP unit costs, sustained:   SFXREVERB 21% each · ECHO 6.7% each · FLANGE 2.6%
```

In a sewer firefight (many voices *and* a wide reverb) the mixer can still run out of time and a small artifact slips through. **Accepted:** rare, minor, and every alternative was worse (see [Rejected](#rejected)).

---

## How the game uses FMOD

Established by hooking the calls and reading `DP.exe`'s import table. Relevant to any future work on the audio.

### Loading

One creation path, `System::createSound`. It imports neither `createStream` nor the C API.

| | Per session | Form |
|---|---|---|
| Samples | 2189 | `OPENMEMORY \| CREATECOMPRESSEDSAMPLE`, mode `0x0a000a41`. **FSB4 banks, one MPEG sample each** |
| Streams | 141 | `CREATESTREAM \| NONBLOCKING` from files, mode `0x0a0100c1` |

Streams are dialogue (`WL_DX_*.xxx`, 18-162 KB) and music (`WL_MX_*.xxx`, 2-2.3 MB), made by passing the flag to `createSound`; `createStream` is never called.

Rates: `(48000,1)` ×868 · `(24000,1)` ×382 · `(48000,2)` ×658 · `(44100,1)` ×4 · `(16000,1)` ×3.

### Configuration

```
setSoftwareChannels     = 64          (peak actually used: 58)
init                      maxchannels=96, flags=0x00000000
setAdvancedSettings       maxMPEGcodecs=64, defaultDecodeBufferSize=0
FMOD_Memory_Initialize    poolmem=NULL, poollen=0        <- no fixed pool
setDSPBufferSize        <- NEVER CALLED (we pin it to 1024x4)
setSoftwareFormat       <- NEVER CALLED (FMOD lands on 48 kHz / LINEAR / float: already optimal)
```

### DSPs

All eighteen attach through `ChannelGroup::addDSP`. None through `Channel::addDSP` or `System::addDSP`, though the game imports all three.

```
4x ECHO · 2x SFXREVERB · 1x FLANGE        <- delay lines, the expensive ones
5x PARAMEQ · 2x LOWPASS · LOWPASS_SIMPLE · HIGHPASS · DISTORTION
3x "Peak Meter" · "McDSP ML1 Compressor" · "McDSP Futzbox Lo-Fi Distortion"   <- the game's own
```

### Playback

Every `playSound` is issued `paused=1`, on an **explicit channel index**: the game hand-manages its 64 channels and never passes `FMOD_CHANNEL_FREE`. It schedules voices against the DSP clock with `setDelay` + `getDSPClock`: 240 calls in one session, **0 in another, with the artifacts present in both**, so that is not a necessary condition for anything.

---

## Dead ends

| Hypothesis | Test | Verdict |
|---|---|---|
| Wine WASAPI output | Force DSOUND / WINMM | ✗ DSOUND worse, WINMM same |
| Speaker/surround misdetected | Log `getDriverCaps`/`setSpeakerMode` | ✗ Stereo correct throughout |
| Wine audio backend | Minimal WASAPI repro in the bottle | ✗ Clean |
| Underruns replaying old data | Force 120 ms stalls | ✗ Clicks and cuts, never echoes |
| Broken clocks (QPC/`timeGetTime`) | Measure them in the bottle | ✗ Accurate |
| Voice starvation → relaunches | Force 512 channels / 256 software voices | ✗ No change |
| The same sound retriggering | Dedup `playSound` by `Sound*` | ✗ Echoes intact |
| Insufficient MPEG codec pool | `maxMPEGcodecs` 64→256 | ✗ `FMOD_ERR_MEMORY` (32-bit process) |
| A newer FMOD fixes it | FMOD Ex 4.44.64 | ✗ Identical echoes |
| Un-hooked creation paths (`createStream`, C API) | `DP.exe` imports | ✗ Only `createSound` exists |
| A **fixed FMOD memory pool** the fix exhausts | Hook `FMOD_Memory_Initialize` | ✗ `poolmem=NULL, poollen=0`. **0 failures** in 46 min |
| Pressure **grows with time in the level** (a sound leak) | Live-sound counter | ✗ Plateaus at ~1633 and stays. What grows is **combat density → voice count** |
| `createSound`'s 82 ms is **mixer lock contention** | Size correlation, and moving the mixer buffer | ✗ **r = 0.929** with size, so it is decode. Moving the mixer changed it by **0 ms** |
| Wine leaves the **DSP buffer** too small | `getDSPBufferSize` before `init` | ✗ Already 1024×4, the value we were about to set |
| Dialogue artifacts are **MPEG codec-pool contention** | Convert dialogue streams to PCM | ✗ Artifacts survived. It was the mixer |
| Wasted **resampling**, since the game never sets the software format | `getSoftwareFormat` after `init` | ✗ FMOD already lands on 48 kHz, LINEAR, float. Optimal for the content |
| **Fewer software channels** would free budget | `getChannelsPlaying` | ✗ Peak is **58 of 64**. Cutting the ceiling would steal voices and drop sounds |
| Block all DSP processing | Block `ChannelGroup::addDSP` | ⚠️ *"barely mitigates"*. The right instinct, wrong layer: it killed the good DSPs too. Bug 2 is the surgical version |
| The old `addDSP` block **had a hole** (only ChannelGroup was covered) | Hook all three variants | ✗ **All DSPs attach via ChannelGroup.** The original block did cover them all |
| The audio can be **extracted offline** from the packages | Search all 1787 packages, 6.2 GB, for the exact MPEG payloads | ✗ **0 found**, see [Offline extraction](#offline-extraction-closed-with-evidence) |

### Offline extraction, closed with evidence

The obvious shortcut, pulling the audio out of the game's files and shipping a complete cache without playing at all, **does not work.**

| Finding | |
|---|---|
| Packages are **not compressed** | `packageflags = 0x4` |
| A byte-scan finds **5081 `FSB4` banks**, 409.9 MB | …but only **153 of 2030** sample names (7.5%) overlap with what the game loads, and even those differ in size |
| Searched all **1787 packages, 6.2 GB**, for the exact MPEG payloads of banks the game really loaded | **0 found** |
| `SoundNodeWave` appears in 192 packages | All `WL_DX_*` / `WL_MX_*`: the **streams**. The class is `SoundNodeWaveEx`, a studio-custom subclass |
| `CompressedPCData` | **Absent.** Deadpool does not use UE3's standard property |
| UE3 chunk-compression magic | **Absent.** Not standard UE3 bulk-data compression either |

**The audio the game hands FMOD does not exist verbatim anywhere on disk.** It is decompressed or transformed at load, by a scheme that is none of UE3's standard ones. Doing this offline means reverse-engineering a **licensee 181** UE3 header (fields inserted after the version, so a naive parser derails at byte 8), then the export table, then an unknown bulk-data encoding.

**If someone picks it up:** the one design change that makes it possible is to **re-key the cache on the MPEG payload** instead of the whole FSB buffer. The game builds the FSB4 header at runtime and it is nowhere in the packages, so an offline extractor could never reproduce it. But if both sides hash only the audio (`buffer[128:]` at runtime, the extracted MPEG offline), they reach the same key without anyone rebuilding that header.

---

## Rejected

Approaches that worked but did not justify their cost.

**Adaptive load shedding.** Watch the mixer's CPU and drop the reverb when it climbs past a threshold, restore it when it falls. It works, and it would have removed the last sewer artifacts. Cut anyway: thresholds, hysteresis, a dead band and a dwell timer, all so a rare minor artifact becomes a rare minor reverb dropout. Not worth the complexity.

**Cutting an effect to buy budget.** The cheapest effect (flange, 2.6%) is the most characterful; the most expensive (reverb, 21%) is what makes a sewer sound like a sewer. Cutting by "least valuable" saves nothing worth having.

**Lowering the software channel count.** Peak concurrent voices is 58 of 64. There is no ceiling to cut.

---

## Constraints and traps

- The game checks `getVersion == 4.36.02`. **A different FMOD version silently disables all audio.**
- FMOD 4.44.64 changes the ABI of `DSPConnection::setMix` (two floats → one). It would need a bridge in the proxy.
- **32-bit address space is a real ceiling.** The samples already take ~387 MB as PCM. Music stays a stream because a 2 MB track costs ~25 MB decoded.
- The in-memory sounds are **FSB4 containers**, not flat sounds. `Sound::lock` on the container returns `pcmbytes=0`; the audio is in its subsound. Anything wanting the decoded PCM back out of FMOD has to go through the subsound.
- **Silencing a DSP does not stop it costing CPU.** FMOD runs it regardless of its wet mix. This was the whole of bug 2, and it is the least obvious thing here. The game's own three `Peak Meter` DSPs are the same trap: wired in, silent, costing the mixer every sample. Dead instrumentation nobody removed is part of what caused bug 2, and the reason to leave none behind.
- FMOD Ex C++ methods are `__stdcall` with `this` as the first stack arg. A hook is a `__stdcall` C function with a leading `void *this` and an `asm("_name")` label so the `.def` binding resolves. Non-obvious, and the whole proxy rests on it.
- **A build that cannot reproduce the bug is worse than none.** Logging `playSound` (tens of thousands of calls, flushed) slowed the game enough to *hide the very artifacts being chased*, and cost a day. Measure with a hook narrow enough not to change the timing, then delete it.
