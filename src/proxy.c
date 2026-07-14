/* fmodex.dll proxy for Deadpool (2013) under Wine/CrossOver. See TECHNICAL.md.
 *
 * Three fixes for two bugs:
 *   1. Realtime MPEG decode misses its deadline under Rosetta, so sounds echo.
 *      Decode to PCM at load instead. (mode.h)
 *   2. That load decode is slow and repeats every launch. Cache the PCM to disk.
 *      (cache, below)
 *   3. FMOD charges full price for a silent DSP; five left connected overload the
 *      mixer into noise. Bypass whatever has gone silent. (fx_update_bypass)
 */

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "mode/mode.h"

typedef int FRESULT; /* FMOD_RESULT; 0 == FMOD_OK */

/* FMOD Ex C++ methods are __stdcall with `this` as the first stack arg, so a
 * plain __stdcall C function with an extra void* first param matches the ABI. */

#define N_CREATESOUND  "?createSound@System@FMOD@@QAG?AW4FMOD_RESULT@@PBDIPAUFMOD_CREATESOUNDEXINFO@@PAPAVSound@2@@Z"
#define N_INIT         "?init@System@FMOD@@QAG?AW4FMOD_RESULT@@HIPAX@Z"
#define N_DSPBUFSIZE   "?setDSPBufferSize@System@FMOD@@QAG?AW4FMOD_RESULT@@IH@Z"
#define N_DSPBYTYPE    "?createDSPByType@System@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_DSP_TYPE@@PAPAVDSP@2@@Z"
#define N_SETPARAM     "?setParameter@DSP@FMOD@@QAG?AW4FMOD_RESULT@@HM@Z"
#define N_SETBYPASS    "?setBypass@DSP@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z"

/* ----------------------------------------------------------------- constants
 *
 * Tuned once and compiled in; the DLL takes no configuration.
 */

/* The game never sets this, so FMOD derives it from the output device at init.
 * Pinning it gives the mixer four blocks of slack to run briefly long. ~21 ms. */
#define DSP_BUFLEN 1024
#define DSP_NUMBUF 4

/* Small dialogue streams become PCM to get them off realtime decode; music
 * (2 MB+) stays streamed, since a 32-bit process cannot hold ~25 MB per track. */
#define STREAM_PCM_MAX (512u * 1024u)

/* Per-DSP parameter index that signals silence, so we can bypass it. */
#define ECHO_WETMIX     4       /* FMOD_DSP_TYPE_ECHO */
#define FLANGE_WETMIX   1       /* FMOD_DSP_TYPE_FLANGE */
#define SFXREVERB_ROOM  1       /* FMOD_DSP_TYPE_SFXREVERB, in mB */

/* ------------------------------------------------------------------ plumbing */

static HMODULE g_orig;
static CRITICAL_SECTION g_cs;
static HINSTANCE g_module; /* set in DllMain */

static FARPROC orig_fn(const char *name)
{
    if (!g_orig) {
        EnterCriticalSection(&g_cs);
        if (!g_orig)
            g_orig = LoadLibraryA("fmodex_og.dll");
        LeaveCriticalSection(&g_cs);
    }
    return g_orig ? GetProcAddress(g_orig, name) : NULL;
}

/* Resolve once per hook rather than once per call. The race is benign: both
 * threads write the same pointer. */
#define ORIG(fn, type, name) \
    static type fn; \
    if (!(fn)) (fn) = (type)orig_fn(name); \
    if (!(fn)) return -1

/* --------------------------------------------------------------- the DSP graph
 *
 * FMOD does not skip a DSP because its wet mix is zero: it processes every sample
 * through it and charges full price. The game leaves five delay-line DSPs
 * connected after ramping them down to silence, which is most of bug 2.
 *
 * The rule: whatever is contributing nothing right now comes out of the graph,
 * whoever silenced it, and goes back the moment it has something to say.
 */

#define DSP_MAX 64
static struct {
    void *p;
    const char *name;
    int silent;
} g_dsp[DSP_MAX];
static int g_ndsp;

static const char *dsp_type_name(int t)
{
    /* Only the three that matter need naming; the rest are cheap and stay put. */
    switch (t) {
    case 6:  return "ECHO";
    case 7:  return "FLANGE";
    case 17: return "SFXREVERB";
    default: return "";
    }
}

/* Which parameter decides whether a delay-line DSP is contributing anything, and
 * the value at or below which it stops. -1 for every other DSP type. */
static int fx_silence_param(const char *n, float *threshold)
{
    if (strcmp(n, "ECHO") == 0)      { *threshold = 0.001f;   return ECHO_WETMIX; }
    if (strcmp(n, "FLANGE") == 0)    { *threshold = 0.001f;   return FLANGE_WETMIX; }
    if (strcmp(n, "SFXREVERB") == 0) { *threshold = -9999.0f; return SFXREVERB_ROOM; }
    return -1;
}

static int dsp_find(void *p)
{
    int i;
    for (i = 0; i < g_ndsp; i++)
        if (g_dsp[i].p == p)
            return i;
    return -1;
}

static void set_bypass(void *dsp, int on)
{
    typedef FRESULT (__stdcall *t)(void*, int);
    t f = (t)orig_fn(N_SETBYPASS);
    if (f)
        f(dsp, on);
}

/* The game just changed a parameter. If that took the DSP to silence, drop it out
 * of the graph; if it gave it something to say, put it back. */
static void fx_update_bypass(void *dsp, const char *n, int index, float value)
{
    float threshold;
    int param = fx_silence_param(n, &threshold);
    int i, silent;

    if (param < 0 || index != param)
        return;

    i = dsp_find(dsp);
    silent = value <= threshold;
    if (i < 0 || g_dsp[i].silent == silent)
        return;

    g_dsp[i].silent = silent;
    set_bypass(dsp, silent);
}

/* --------------------------------------------------------------------- cache
 *
 * The game hands createSound a whole FSB4 bank with FMOD_OPENMEMORY, so `name`
 * *is* the compressed data: it addresses itself. audio_cache/ holds the same
 * banks with a PCM sample swapped in for the MPEG one, so a hit leaves FMOD
 * nothing to decode. It fills itself: cache_store_pcm reads back the PCM FMOD
 * produces the first time a bank decodes.
 *
 * A bank is rebuilt, not replaced with bare PCM, because the game asked for a
 * container and the audio lives in its subsound. A miss or a rejected bank
 * decodes as always, so nothing here can silence a sound.
 */

/* Leading fields of FMOD_CREATESOUNDEXINFO; we copy the game's and edit length. */
struct exinfo_head {
    int cbsize;
    unsigned length;
};

static unsigned long long fnv1a(const void *p, unsigned n)
{
    const unsigned char *b = (const unsigned char *)p;
    unsigned long long h = 1469598103934665603ULL;
    unsigned i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int g_cache_on = -1;
static char g_cache_dir[MAX_PATH];

static int cache_ready(void)
{
    if (g_cache_on < 0) {
        char *p;
        EnterCriticalSection(&g_cs);
        if (g_cache_on < 0) {
            g_cache_on = 0;
            if (GetModuleFileNameA(g_module, g_cache_dir, sizeof g_cache_dir) &&
                (p = strrchr(g_cache_dir, '\\')) != NULL) {
                lstrcpyA(p + 1, "audio_cache");
                CreateDirectoryA(g_cache_dir, NULL);
                g_cache_on = 1;
            }
        }
        LeaveCriticalSection(&g_cs);
    }
    return g_cache_on;
}

static void cache_path(char *out, unsigned long long h, const char *ext)
{
    wsprintfA(out, "%s\\%08x%08x.%s", g_cache_dir,
              (unsigned)(h >> 32), (unsigned)h, ext);
}

/* Keep the raw compressed bank: material for decode_cache.py, and the fallback if
 * a .pcm ever goes bad. One write per new sound. */
static void cache_dump_src(unsigned long long h, const void *src, unsigned len)
{
    char path[MAX_PATH];
    FILE *fp;

    cache_path(path, h, "src");
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        return;
    fp = fopen(path, "wb");
    if (!fp)
        return;
    fwrite(src, 1, len, fp);
    fclose(fp);
}

/* A malloc'd copy of the rebuilt PCM bank, or NULL on a miss. */
static void *cache_load(unsigned long long h, unsigned *size)
{
    char path[MAX_PATH];
    FILE *fp;
    long n;
    void *fsb;

    cache_path(path, h, "pcm");
    fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0 || (n = ftell(fp)) <= 128 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    fsb = malloc((size_t)n);
    if (!fsb) {
        fclose(fp);
        return NULL;
    }
    if (fread(fsb, 1, (size_t)n, fp) != (size_t)n || memcmp(fsb, "FSB4", 4) != 0) {
        free(fsb);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *size = (unsigned)n;
    return fsb;
}

/* An OPENMEMORY compressed sample: the bank is right there in `name`. */
static int cacheable(unsigned mode, const struct exinfo_head *ex, unsigned len)
{
    return (mode & FMOD_OPENMEMORY) && (mode & FMOD_CREATECOMPRESSEDSAMPLE) &&
           ex && len && ex->cbsize > 0 && ex->cbsize <= 512;
}

/* FSB4 layout and FSOUND_* sample-mode bits, mirroring tools/decode_cache.py. */
#define FSB_OFF_DATASIZE   12
#define FSB_SH             48
#define FSB_OFF_SHDRSIZE   8
#define FSB_OFF_NSAMPLES   (FSB_SH + 32)
#define FSB_OFF_LENCOMP    (FSB_SH + 36)
#define FSB_OFF_MODE       (FSB_SH + 48)
#define FSB_OFF_CHANNELS   (FSB_SH + 62)
#define FSOUND_16BITS      0x00000010u
#define FSOUND_MONO        0x00000020u
#define FSOUND_STEREO      0x00000040u
#define FSOUND_SIGNED      0x00000100u
#define FSOUND_MPEG_BITS   0x000C0200u   /* MPEG | LAYER3 | LAYER2 */

static unsigned rd32(const unsigned char *b, unsigned off)
{
    unsigned v;
    memcpy(&v, b + off, 4);
    return v;
}

/* Read the PCM FMOD just decoded back out of the sound and write it as a cache
 * entry, so the decode is paid once ever. Failure is harmless: the sound already
 * loaded, and the .src dump stays for the offline decoder. */
static void cache_store_pcm(unsigned long long h, const void *src, unsigned len, void *snd)
{
    typedef FRESULT (__stdcall *tnum)(void*, int*);
    typedef FRESULT (__stdcall *tsub)(void*, int, void**);
    typedef FRESULT (__stdcall *tlock)(void*, unsigned, unsigned, void**, void**, unsigned*, unsigned*);
    typedef FRESULT (__stdcall *tunlock)(void*, void*, void*, unsigned, unsigned);
    const unsigned char *b = (const unsigned char *)src;
    char path[MAX_PATH];
    unsigned shdrsize, nsamples, mode, want, hdr, plen;
    unsigned short nch;
    void *sub = snd, *p1 = NULL, *p2 = NULL;
    unsigned l1 = 0, l2 = 0;
    int n = 0;
    tnum fnum;
    tsub fsub;
    tlock flock;
    tunlock funlock;
    unsigned char *out;
    FILE *fp;

    cache_path(path, h, "pcm");
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        return;
    if (len < FSB_SH + 64 || memcmp(b, "FSB4", 4) != 0)
        return;

    shdrsize = rd32(b, FSB_OFF_SHDRSIZE);
    nsamples = rd32(b, FSB_OFF_NSAMPLES);
    mode     = rd32(b, FSB_OFF_MODE);
    memcpy(&nch, b + FSB_OFF_CHANNELS, 2);
    hdr = FSB_SH + shdrsize;
    if (!nch || !nsamples || hdr > len)
        return;
    want = nsamples * (unsigned)nch * 2;

    flock = (tlock)orig_fn("FMOD_Sound_Lock");
    funlock = (tunlock)orig_fn("FMOD_Sound_Unlock");
    if (!flock || !funlock)
        return;

    /* The PCM lives in the container's subsound, not the container itself:
     * locking the container is the read that came back empty. */
    fnum = (tnum)orig_fn("FMOD_Sound_GetNumSubSounds");
    fsub = (tsub)orig_fn("FMOD_Sound_GetSubSound");
    if (fnum && fsub && fnum(snd, &n) == 0 && n > 0) {
        void *s = NULL;
        if (fsub(snd, 0, &s) == 0 && s)
            sub = s;
    }

    if (flock(sub, 0, want, &p1, &p2, &l1, &l2) != 0 || !p1 || !l1) {
        funlock(sub, p1, p2, l1, l2);
        return;
    }

    out = (unsigned char *)malloc(hdr + want);
    if (out) {
        plen = l1 < want ? l1 : want;
        memcpy(out, b, hdr);
        memcpy(out + hdr, p1, plen);
        memset(out + hdr + plen, 0, want - plen);   /* pad short frames like the decoder */
        mode = (mode & ~FSOUND_MPEG_BITS) | FSOUND_16BITS | FSOUND_SIGNED |
               (nch >= 2 ? FSOUND_STEREO : FSOUND_MONO);
        memcpy(out + FSB_OFF_DATASIZE, &want, 4);
        memcpy(out + FSB_OFF_LENCOMP, &want, 4);
        memcpy(out + FSB_OFF_MODE, &mode, 4);

        fp = fopen(path, "wb");
        if (fp) {
            fwrite(out, 1, hdr + want, fp);
            fclose(fp);
        }
        free(out);
    }
    funlock(sub, p1, p2, l1, l2);
}

/* ---------------------------------------------------------------------- hooks */

FRESULT __stdcall hook_System_createSound(void *sys, const char *name, unsigned mode, void *exinfo, void **snd) asm("_hook_System_createSound");
FRESULT __stdcall hook_System_createSound(void *sys, const char *name, unsigned mode, void *exinfo, void **snd)
{
    typedef FRESULT (__stdcall *t)(void*, const char*, unsigned, void*, void**);
    struct exinfo_head *ex = (struct exinfo_head *)exinfo;
    unsigned len = (ex && ex->cbsize >= (int)sizeof *ex) ? ex->length : 0;
    unsigned in_mode = mode;
    unsigned long long h;
    unsigned fsb_len;
    void *fsb;
    FRESULT r;

    ORIG(f, t, N_CREATESOUND);

    mode = dp_fix_mode(mode, len, STREAM_PCM_MAX);

    if (!cacheable(in_mode, ex, len) || !cache_ready())
        return f(sys, name, mode, exinfo, snd);

    h = fnv1a(name, len);
    cache_dump_src(h, name, len);

    fsb = cache_load(h, &fsb_len);
    if (fsb) {
        char buf[512];
        struct exinfo_head *e2 = (struct exinfo_head *)buf;

        /* The same call the game made, pointed at the PCM bank instead. Only the
         * length changes: FMOD reads the format out of the bank itself. */
        memcpy(buf, ex, ex->cbsize);
        e2->length = fsb_len;

        r = f(sys, (const char *)fsb, mode, buf, snd);
        free(fsb);
        if (r == 0)
            return r;
        /* A bank FMOD rejects costs nothing: fall through and decode. */
    }

    /* Fresh decode: capture the PCM FMOD just produced so future launches hit. */
    r = f(sys, name, mode, exinfo, snd);
    if (r == 0 && snd && *snd)
        cache_store_pcm(h, name, len, *snd);
    return r;
}

FRESULT __stdcall hook_System_init(void *sys, int maxchannels, unsigned flags, void *extra) asm("_hook_System_init");
FRESULT __stdcall hook_System_init(void *sys, int maxchannels, unsigned flags, void *extra)
{
    typedef FRESULT (__stdcall *tinit)(void*, int, unsigned, void*);
    typedef FRESULT (__stdcall *tset)(void*, unsigned, int);
    tset fset;

    ORIG(f, tinit, N_INIT);

    fset = (tset)orig_fn(N_DSPBUFSIZE);
    if (fset) /* must happen before init or FMOD ignores it */
        fset(sys, DSP_BUFLEN, DSP_NUMBUF);
    return f(sys, maxchannels, flags, extra);
}

FRESULT __stdcall hook_System_createDSPByType(void *sys, int type, void **dsp) asm("_hook_System_createDSPByType");
FRESULT __stdcall hook_System_createDSPByType(void *sys, int type, void **dsp)
{
    typedef FRESULT (__stdcall *t)(void*, int, void**);
    const char *n = dsp_type_name(type);
    float threshold;
    int is_delay = fx_silence_param(n, &threshold) >= 0;
    FRESULT r;

    ORIG(f, t, N_DSPBYTYPE);

    r = f(sys, type, dsp);
    if (r == 0 && is_delay && dsp && *dsp) {
        /* Delay-line DSPs start outside the graph. The game puts each one in by
         * giving it a wet mix, which fx_update_bypass notices. */
        EnterCriticalSection(&g_cs);
        if (g_ndsp < DSP_MAX) {
            g_dsp[g_ndsp].p = *dsp;
            g_dsp[g_ndsp].name = n;
            g_dsp[g_ndsp].silent = 1;
            g_ndsp++;
        }
        LeaveCriticalSection(&g_cs);
        set_bypass(*dsp, 1);
    }
    return r;
}

FRESULT __stdcall hook_DSP_setParameter(void *dsp, int index, float value) asm("_hook_DSP_setParameter");
FRESULT __stdcall hook_DSP_setParameter(void *dsp, int index, float value)
{
    typedef FRESULT (__stdcall *t)(void*, int, float);
    int i = dsp_find(dsp);
    const char *n = i < 0 ? "" : g_dsp[i].name;
    FRESULT r;

    ORIG(f, t, N_SETPARAM);

    r = f(dsp, index, value);
    if (r == 0)
        fx_update_bypass(dsp, n, index, value);
    return r;
}

/* The game bypasses and un-bypasses these as it goes. Its "on" only counts if the
 * DSP has something to say. */
FRESULT __stdcall hook_DSP_setBypass(void *dsp, int bypass) asm("_hook_DSP_setBypass");
FRESULT __stdcall hook_DSP_setBypass(void *dsp, int bypass)
{
    typedef FRESULT (__stdcall *t)(void*, int);
    int i = dsp_find(dsp);

    ORIG(f, t, N_SETBYPASS);

    if (i >= 0 && g_dsp[i].silent)
        bypass = 1;
    return f(dsp, bypass);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        g_module = inst;
    }
    return TRUE;
}
