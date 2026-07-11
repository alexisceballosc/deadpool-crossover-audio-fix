#!/usr/bin/env python3
"""Generate proxy.def: hooks for intercepted exports, forwarders for the rest."""

from pathlib import Path

HERE = Path(__file__).resolve().parent
EXPORTS = HERE / 'exports.txt'
OUT = HERE.parent / 'proxy.def'

INTERCEPT = {
    '?createSound@System@FMOD@@QAG?AW4FMOD_RESULT@@PBDIPAUFMOD_CREATESOUNDEXINFO@@PAPAVSound@2@@Z': 'hook_System_createSound',
}

names = EXPORTS.read_text().split()
lines = ['LIBRARY fmodex.dll', 'EXPORTS']
hit = set()
for n in names:
    if n in INTERCEPT:
        lines.append('    "%s" = %s' % (n, INTERCEPT[n]))
        hit.add(n)
    else:
        lines.append('    "%s" = "fmodex_og.%s"' % (n, n))

missing = set(INTERCEPT) - hit
if missing:
    raise SystemExit('intercepted names not in export list: %r' % missing)

OUT.write_text('\n'.join(lines) + '\n')
print('%s: %d exports, %d hooked' % (OUT, len(names), len(hit)))
