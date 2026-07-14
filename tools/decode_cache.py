#!/usr/bin/env python3
"""Decode the .src banks the proxy dumped and rebuild them with PCM inside.

Each .src is an FSB4 bank holding one MPEG sample. Decode it natively (~48x
faster than the game under Rosetta) and write back the same bank with a PCM
sample, keeping the container and its subsound. The output keeps the .src's
name, which is the hash the proxy keys on.
"""
import glob, os, struct, subprocess, sys
from concurrent.futures import ProcessPoolExecutor

CACHE = sys.argv[1] if len(sys.argv) > 1 else "."

# FSB sample mode bits (FSOUND_*).
FSOUND_16BITS      = 0x00000010
FSOUND_MONO        = 0x00000020
FSOUND_STEREO      = 0x00000040
FSOUND_SIGNED      = 0x00000100
FSOUND_MPEG        = 0x00000200
FSOUND_MPEG_LAYER3 = 0x00040000
FSOUND_MPEG_LAYER2 = 0x00080000
MPEG_BITS = FSOUND_MPEG | FSOUND_MPEG_LAYER3 | FSOUND_MPEG_LAYER2

# Offsets inside the 80-byte sample header, which starts at 48.
SH = 48
OFF_LENSAMPLES = SH + 32
OFF_LENCOMP    = SH + 36
OFF_MODE       = SH + 48
OFF_FREQ       = SH + 52
OFF_CHANNELS   = SH + 62


def decode(path):
    d = bytearray(open(path, 'rb').read())
    if d[:4] != b'FSB4':
        return path, 'no es FSB4'

    shdrsize = struct.unpack_from('<i', d, 8)[0]
    nsamples = struct.unpack_from('<I', d, OFF_LENSAMPLES)[0]
    mode     = struct.unpack_from('<I', d, OFF_MODE)[0]
    freq     = struct.unpack_from('<i', d, OFF_FREQ)[0]
    nch      = struct.unpack_from('<H', d, OFF_CHANNELS)[0]
    mpeg     = bytes(d[48 + shdrsize:])

    p = subprocess.run(
        ['ffmpeg', '-v', 'quiet', '-f', 'mp3', '-i', 'pipe:0',
         '-f', 's16le', '-acodec', 'pcm_s16le', '-ar', str(freq), '-ac', str(nch), 'pipe:1'],
        input=mpeg, capture_output=True)
    pcm = p.stdout
    if p.returncode != 0 or not pcm:
        return path, f'ffmpeg fallo rc={p.returncode}'

    # The bank's own sample count is the authority: ffmpeg trims the MPEG encoder
    # delay, which leaves some files a frame short. Pad or clip to the length the
    # game expects.
    want = nsamples * nch * 2
    pcm = pcm[:want] if len(pcm) > want else pcm + b'\x00' * (want - len(pcm))

    # Same headers, PCM sample: only what describes the encoding changes.
    out = bytearray(d[:48 + shdrsize])
    struct.pack_into('<i', out, 12, len(pcm))           # FSB header datasize
    struct.pack_into('<I', out, OFF_LENCOMP, len(pcm))  # sample's stored bytes
    new_mode = (mode & ~MPEG_BITS) | FSOUND_16BITS | FSOUND_SIGNED
    new_mode |= FSOUND_STEREO if nch >= 2 else FSOUND_MONO
    struct.pack_into('<I', out, OFF_MODE, new_mode)
    out += pcm

    open(path[:-4] + '.pcm', 'wb').write(bytes(out))
    return path, len(out)


if __name__ == '__main__':
    srcs = sorted(glob.glob(os.path.join(CACHE, '*.src')))
    print(f"reconstruyendo {len(srcs)} bancos FSB con PCM...")
    ok = bad = total = 0
    with ProcessPoolExecutor() as ex:
        for path, r in ex.map(decode, srcs):
            if isinstance(r, int):
                ok += 1
                total += r
            else:
                bad += 1
                if bad <= 5:
                    print(f"  FALLO {os.path.basename(path)}: {r}")
    print(f"\nok={ok}  fallidos={bad}  cache={total/1048576:.1f} MB")
