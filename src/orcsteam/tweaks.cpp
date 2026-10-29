// Small game options from orcsteam.ini (hooks with MinHook, third_party\minhook):
// skip_intro=1  the boot logos (Capcom, Slant Six, Hexane) and the NVIDIA movie go by in a blink. The boot flow
//               (0x7A1F70) queues each logo through 0x45C150(name, seconds) and the movie through
//               0x45B480(file, seconds); nothing else calls them, so a near-zero time there only shortens the intro.
// fov=<percent> every camera's vertical field of view times this (100 = the game's own; 120 = 20% wider).
//               The camera settings register "/Camera/<who>/<move>/<view>/Y Fov" through the tweak system
//               (0xAA0A60(path, float*, min, max, flags)); the values come from tweaks\globaltweaks.tbin later, so
//               the pump thread scales whatever the game has put there since its last look.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "../../third_party/minhook/include/MinHook.h"
#include "overrides.h"

static const DWORD kRvaBootLogo = 0x05C150, kRvaBootMovie = 0x05B480, kRvaTweakFloat = 0x6A0A60;
static const BYTE kPrologBootLogo[] = {0x55, 0x8B, 0xEC, 0xD9, 0x45, 0x0C, 0x56, 0x83, 0xEC, 0x08, 0x8B, 0xF1};
static const BYTE kPrologBootMovie[] = {0x55, 0x8B, 0xEC, 0xF3, 0x0F, 0x10, 0x45, 0x0C, 0x8B, 0x45, 0x08, 0x51};
static const BYTE kPrologTweakFloat[] = {0x55, 0x8B, 0xEC, 0x8B, 0x0D};

static const float kIntroSeconds = 0.05f;
static const int kMaxFov = 256;  // more than 64 in a versus match
static const float kFovMin = 10.0f, kFovMax = 170.0f;

// Both are __thiscall; a __fastcall detour gets this in ecx and an unused edx.
typedef void(__thiscall* BootLogoFn)(void*, void* name, float seconds);  // name: the game's ref-counted string
typedef void(__thiscall* BootMovieFn)(void*, const char* file, float seconds);
typedef void(__cdecl* TweakFloatFn)(const char* path, float* value, float min, float max, int flags);
static BootLogoFn g_origBootLogo;
static BootMovieFn g_origBootMovie;
static TweakFloatFn g_origTweakFloat;

struct Fov { float* value; float written; };
static SRWLOCK g_lock = SRWLOCK_INIT;
static Fov g_fovs[kMaxFov];
static int g_fovCount;
static float g_fovScale = 1.0f;

static void __fastcall HookBootLogo(void* self, void*, void* name, float) {
    g_origBootLogo(self, name, kIntroSeconds);
}

static void __fastcall HookBootMovie(void* self, void*, const char* file, float) {
    g_origBootMovie(self, file, kIntroSeconds);
}

static bool IsCameraFov(const char* path) {
    size_t n = strlen(path);
    return !strncmp(path, "/Camera/", 8) && n > 6 && !strcmp(path + n - 6, "/Y Fov");
}

static void __cdecl HookTweakFloat(const char* path, float* value, float min, float max, int flags) {
    g_origTweakFloat(path, value, min, max, flags);
    if (!path || !value || !IsCameraFov(path)) return;
    AcquireSRWLockExclusive(&g_lock);
    if (g_fovCount < kMaxFov) g_fovs[g_fovCount++] = {value, 0.0f};
    else if (g_fovCount++ == kMaxFov) OverrideLog("!! tweaks: more than %d camera views, the rest keep their field of view", kMaxFov);
    ReleaseSRWLockExclusive(&g_lock);
}

static bool Hook(BYTE* base, DWORD rva, const BYTE* prolog, size_t n, void* detour, void** original, const char* name) {
    BYTE* target = base + rva;
    if (memcmp(target, prolog, n) != 0) {
        OverrideLog("!! tweaks: %s does not look as expected, not hooked", name);
        return false;
    }
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
        OverrideLog("!! tweaks: hooking %s failed", name);
        return false;
    }
    return true;
}

// DllMain: before the exe runs, so the boot flow and every tweak registration come later.
void GameTweaksInstall() {
    bool skipIntro = OrcsteamIniInt("skip_intro", 0) == 1;
    int fov = OrcsteamIniInt("fov", 100);
    if (fov < 50) fov = 50;
    if (fov > 200) fov = 200;
    if (!skipIntro && fov == 100) return;
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* file = strrchr(exe, '\\');
    if (_stricmp(file ? file + 1 : exe, "RaccoonCity.exe") != 0) return;  // self-test
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        OverrideLog("!! tweaks: MinHook init failed");
        return;
    }
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    if (skipIntro &&
        Hook(base, kRvaBootLogo, kPrologBootLogo, sizeof(kPrologBootLogo), HookBootLogo, (void**)&g_origBootLogo, "boot logo") &&
        Hook(base, kRvaBootMovie, kPrologBootMovie, sizeof(kPrologBootMovie), HookBootMovie, (void**)&g_origBootMovie, "boot movie"))
        OverrideLog("tweaks: intro skipped (skip_intro=1)");
    if (fov != 100 &&
        Hook(base, kRvaTweakFloat, kPrologTweakFloat, sizeof(kPrologTweakFloat), HookTweakFloat, (void**)&g_origTweakFloat, "tweak float")) {
        g_fovScale = fov / 100.0f;
        OverrideLog("tweaks: camera field of view %d%% (fov=%d)", fov, fov);
    }
}

// Pump thread, every 250 ms: a value the game has (re)loaded since we last wrote it is a new base.
void GameTweaksTick() {
    if (g_fovScale == 1.0f) return;
    static bool logged;
    int scaled = 0;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < g_fovCount && i < kMaxFov; i++) {
        Fov& f = g_fovs[i];
        float now = *f.value;
        if (now == f.written || now < kFovMin || now > kFovMax) continue;
        float want = now * g_fovScale;
        f.written = want < kFovMin ? kFovMin : want > kFovMax ? kFovMax : want;
        *f.value = f.written;
        scaled++;
    }
    int count = g_fovCount;
    ReleaseSRWLockShared(&g_lock);
    if (scaled && !logged) {
        logged = true;
        OverrideLog("tweaks: %d of %d camera views scaled", scaled, count);
    }
}
