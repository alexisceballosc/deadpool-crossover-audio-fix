/* cc -o /tmp/t test.c && /tmp/t */

#include <assert.h>
#include <stdio.h>
#include "mode.h"

#define CAP (512u * 1024u)

int main(void)
{
    /* The two modes Deadpool actually asks for, from fmodex_proxy.log. */
    unsigned sample = 0x0a000a41; /* LOWMEM|IGNORETAGS|OPENMEMORY|COMPRESSED|SOFTWARE|LOOP_OFF */
    unsigned stream = 0x0a0100c1; /* LOWMEM|IGNORETAGS|NONBLOCKING|STREAM|SOFTWARE|LOOP_OFF */

    /* Compressed sample -> PCM sample, every other flag preserved. */
    assert(dp_fix_mode(sample, 27392, CAP) == 0x0a000941);

    /* Dialogue stream (largest seen: 162368 B) -> PCM sample. */
    assert(dp_fix_mode(stream, 162368, CAP) == 0x0a010141);

    /* Music stream (2 MB+) stays a stream: as PCM it would cost ~25 MB. */
    assert(dp_fix_mode(stream, 2013248, CAP) == stream);

    /* Exactly at the cap stays a stream (cutoff is strict). */
    assert(dp_fix_mode(stream, CAP, CAP) == stream);

    /* No length means no way to bound the PCM cost, so don't touch it. */
    assert(dp_fix_mode(stream, 0, CAP) == stream);

    /* cap == 0 disables stream conversion but not the sample conversion. */
    assert(dp_fix_mode(stream, 1024, 0) == stream);
    assert(dp_fix_mode(sample, 1024, 0) == 0x0a000941);

    /* Never emit both flags at once: they are mutually exclusive in FMOD. */
    assert(!(dp_fix_mode(stream, 1024, CAP) & FMOD_CREATESTREAM));
    assert(!(dp_fix_mode(sample, 1024, CAP) & FMOD_CREATECOMPRESSEDSAMPLE));

    puts("mode: ok");
    return 0;
}
