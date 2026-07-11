/* fmodex.dll proxy for Deadpool (2013). See README.md. */

#include <windows.h>

typedef int FRESULT; /* FMOD_RESULT */

/* FMOD Ex C++ methods are __stdcall with `this` as the first stack arg,
 * so a plain __stdcall C function with an extra void* first param
 * matches the ABI. */

#define N_CREATESOUND "?createSound@System@FMOD@@QAG?AW4FMOD_RESULT@@PBDIPAUFMOD_CREATESOUNDEXINFO@@PAPAVSound@2@@Z"
#define FMOD_CREATECOMPRESSEDSAMPLE 0x200
#define FMOD_CREATESAMPLE           0x100

static HMODULE g_orig;
static CRITICAL_SECTION g_cs;

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

FRESULT __stdcall hook_System_createSound(void *sys, const char *name, unsigned mode, void *exinfo, void **snd) asm("_hook_System_createSound");
FRESULT __stdcall hook_System_createSound(void *sys, const char *name, unsigned mode, void *exinfo, void **snd)
{
    typedef FRESULT (__stdcall *t)(void*, const char*, unsigned, void*, void**);
    t f = (t)orig_fn(N_CREATESOUND);
    if (mode & FMOD_CREATECOMPRESSEDSAMPLE)
        mode = (mode & ~(unsigned)FMOD_CREATECOMPRESSEDSAMPLE) | FMOD_CREATESAMPLE;
    return f ? f(sys, name, mode, exinfo, snd) : -1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
    }
    return TRUE;
}
