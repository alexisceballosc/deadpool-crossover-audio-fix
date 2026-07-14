/* createSound flag rewrite, split out so it can be tested without Windows.
 * See test.c. */

#ifndef DP_MODE_H
#define DP_MODE_H

#define FMOD_CREATESTREAM           0x080
#define FMOD_CREATESAMPLE           0x100
#define FMOD_CREATECOMPRESSEDSAMPLE 0x200
#define FMOD_OPENMEMORY             0x800

/* Rewrite a createSound mode to decode PCM at load instead of MPEG in realtime
 * (the echo fix).
 *
 *   len: exinfo length, 0 if the game gave none.
 *   cap: streams strictly smaller than this become samples; 0 disables that.
 *
 * Streams only convert under cap: a 2 MB music track costs ~25 MB as PCM, which
 * a 32-bit process cannot afford. */
static unsigned dp_fix_mode(unsigned mode, unsigned len, unsigned cap)
{
    if (mode & FMOD_CREATECOMPRESSEDSAMPLE)
        mode = (mode & ~(unsigned)FMOD_CREATECOMPRESSEDSAMPLE) | FMOD_CREATESAMPLE;

    if ((mode & FMOD_CREATESTREAM) && cap && len && len < cap)
        mode = (mode & ~(unsigned)FMOD_CREATESTREAM) | FMOD_CREATESAMPLE;

    return mode;
}

#endif
