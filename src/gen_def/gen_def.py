#!/usr/bin/env python3
"""Generate proxy.def: hooks for the exports we intercept, forwarders to the real
FMOD for the other 693.

Paths resolve relative to this file, so it runs from anywhere.
"""
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXPORTS = HERE / 'exports.txt'
OUT = HERE.parent / 'proxy.def'

INTERCEPT = {
    # Decode compressed samples to PCM at load, and serve them from the cache.
    '?createSound@System@FMOD@@QAG?AW4FMOD_RESULT@@PBDIPAUFMOD_CREATESOUNDEXINFO@@PAPAVSound@2@@Z': 'hook_System_createSound',
    # Pin the DSP buffer before FMOD re-derives one from the output device.
    '?init@System@FMOD@@QAG?AW4FMOD_RESULT@@HIPAX@Z': 'hook_System_init',
    # Track which DSP pointers are delay lines.
    '?createDSPByType@System@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_DSP_TYPE@@PAPAVDSP@2@@Z': 'hook_System_createDSPByType',
    # Bypass a delay DSP once it goes silent, and hold it there.
    '?setParameter@DSP@FMOD@@QAG?AW4FMOD_RESULT@@HM@Z': 'hook_DSP_setParameter',
    '?setBypass@DSP@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z': 'hook_DSP_setBypass',
}

names = EXPORTS.read_text().split()
missing = set(INTERCEPT) - set(names)
if missing:
    raise SystemExit('intercepted names not in export list: %r' % missing)

lines = ['LIBRARY fmodex.dll', 'EXPORTS']
for n in names:
    if n in INTERCEPT:
        lines.append('    "%s" = %s' % (n, INTERCEPT[n]))
    else:
        lines.append('    "%s" = "fmodex_og.%s"' % (n, n))

OUT.write_text('\n'.join(lines) + '\n')
print('%s: %d exports, %d hooked' % (OUT.name, len(names), len(INTERCEPT)))
