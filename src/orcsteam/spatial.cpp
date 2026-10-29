// Where things are, for spatial voice (1.4): what the game tells its Wwise sound engine.
// The exe exports Wwise, so a few hooks (MinHook, third_party\minhook) see every sound object with its debug
// name, its position and its acoustic environments (old Wwise "environments" = reverb zones), and the names
// the game hashes into Wwise IDs (GetIDFromString), which is how a zone's name can be known. The listener
// (camera) comes from the thread-safe Query API.
// Our own character is the nearest human character object ("SES:<name>", bots end in "_AI") to the
// listener; voice.cpp sends its position and environment along with our voice, and places other players'
// voices from theirs. All positions are in Wwise space (the game negates Z on the way in), the same on
// every machine. With diag=1 a report goes to orcsteam.log every 2 s while the camera moves.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../../third_party/minhook/include/MinHook.h"
#include "overrides.h"

// 3D emitters get their positions through SetMultiplePositions (the game's emitter update at 0x970DDD);
// SetPosition only parks 2D sounds at the origin (0x970C1F). Seen in the 1.3 test logs.
static const DWORD kRvaSetPosition = 0x5123C0, kRvaSetMultiple = 0x510C40, kRvaRegisterObj = 0x5116C0,
                   kRvaUnregisterObj = 0x511710, kRvaSetEnvValues = 0x511120, kRvaGetIdFromString = 0x512330,
                   kRvaGetListenerPosition = 0x8655D0;
// First bytes of each function in the Steam exe: hooks go in only if they match.
static const BYTE kPrologPosition[] = {0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0};
static const BYTE kPrologMultiple[] = {0x55, 0x8B, 0xEC, 0x51, 0x83, 0x7D, 0x14, 0x02};
static const BYTE kPrologObj[] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x38};
static const BYTE kPrologEnv[] = {0x8B, 0x54, 0x24, 0x0C, 0x83, 0xEC, 0x38};
static const BYTE kPrologGetId[] = {0x81, 0xEC, 0x04, 0x01, 0x00, 0x00, 0x56};
static const BYTE kPrologListener[] = {0x56, 0x68};

static const int kTableSize = 4096;  // open addressing, power of two
static const int kNameTableSize = 4096;
static const int kMaxEnv = 4;
static const DWORD kReportEveryMs = 2000;
static const DWORD kQuietReportMs = 20000;  // without camera movement, a report this often at most
static const DWORD kFreshMs = 1000;         // a character not moved for this long is gone (dead, despawned)

struct AkVector { float x, y, z; };
struct AkSoundPosition { AkVector position, orientation; };
struct AkListenerPosition { AkVector front, top, position; };
struct AkEnvironmentValue { DWORD envId; float value; };

struct Obj {
    DWORD id;  // 0 = free slot
    char name[32];
    bool human;  // "SES:<character>" without "_AI"
    AkVector pos;  // the first one for multi-position emitters
    WORD posCount;
    DWORD moved;
    int envCount;
    AkEnvironmentValue env[kMaxEnv];
    DWORD envSeen;  // checksum of env, logged when it changes for a nearby object
};

struct Name { DWORD id; char text[40]; };

static SRWLOCK g_lock = SRWLOCK_INIT;
static Obj g_objs[kTableSize];
static Name g_names[kNameTableSize];
static volatile LONG g_installed, g_setPositions;
static BYTE* g_base;

typedef int(__cdecl* SetPositionFn)(DWORD, const AkSoundPosition*, DWORD);
typedef int(__cdecl* SetMultipleFn)(DWORD, const AkSoundPosition*, DWORD count, DWORD type);  // count: a WORD pushed as 32 bits
typedef int(__cdecl* RegisterObjFn)(DWORD, const char*);
typedef int(__cdecl* UnregisterObjFn)(DWORD);
typedef int(__cdecl* SetEnvFn)(DWORD, const AkEnvironmentValue*, DWORD);
typedef DWORD(__cdecl* GetIdFn)(const char*);
typedef int(__cdecl* GetListenerFn)(DWORD, AkListenerPosition*);
static SetPositionFn g_origSetPosition;
static SetMultipleFn g_origSetMultiple;
static RegisterObjFn g_origRegisterObj;
static UnregisterObjFn g_origUnregisterObj;
static SetEnvFn g_origSetEnv;
static GetIdFn g_origGetId;
static GetListenerFn g_getListener;

static Obj* Slot(DWORD id, bool create) {  // caller holds g_lock
    DWORD h = (id * 2654435761u) & (kTableSize - 1);
    for (int i = 0; i < kTableSize; i++) {
        Obj& o = g_objs[(h + i) & (kTableSize - 1)];
        if (o.id == id) return &o;
        if (!o.id) {
            if (!create) return nullptr;
            memset(&o, 0, sizeof(o));
            o.id = id;
            return &o;
        }
    }
    return nullptr;
}

// The name argument only exists for the (id, name) overload of RegisterGameObj (both share one body),
// so every name is checked carefully.
static void CopyName(const char* src, char* out, size_t cap) {
    out[0] = 0;
    if ((ULONG_PTR)src < 0x10000) return;
    __try {
        size_t i = 0;
        for (; i + 1 < cap && src[i]; i++) {
            if (src[i] < 0x20 || src[i] > 0x7E) {
                out[0] = 0;
                return;
            }
            out[i] = src[i];
        }
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
}

static bool IsHuman(const char* name) {
    size_t n = strlen(name);
    return n > 4 && !strncmp(name, "SES:", 4) && !(n > 3 && !_stricmp(name + n - 3, "_AI"));
}

static int __cdecl HookRegisterObj(DWORD id, const char* name) {
    char text[32];
    CopyName(name, text, sizeof(text));
    AcquireSRWLockExclusive(&g_lock);
    if (Obj* o = Slot(id, true)) {
        strcpy_s(o->name, text);
        o->human = IsHuman(text);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return g_origRegisterObj(id, name);
}

static int __cdecl HookUnregisterObj(DWORD id) {
    AcquireSRWLockExclusive(&g_lock);
    if (Obj* o = Slot(id, false)) o->moved = 0;  // kept as a tombstone so lookups past it still work
    ReleaseSRWLockExclusive(&g_lock);
    return g_origUnregisterObj(id);
}

static void SetPos(DWORD id, const AkVector& pos, WORD count) {
    AcquireSRWLockExclusive(&g_lock);
    if (Obj* o = Slot(id, true)) {
        o->pos = pos;
        o->posCount = count;
        o->moved = GetTickCount();
    }
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedIncrement(&g_setPositions);
}

static int __cdecl HookSetPosition(DWORD id, const AkSoundPosition* pos, DWORD listener) {
    if (pos) SetPos(id, pos->position, 1);
    return g_origSetPosition(id, pos, listener);
}

static int __cdecl HookSetMultiple(DWORD id, const AkSoundPosition* pos, DWORD count, DWORD type) {
    if (pos && (WORD)count) SetPos(id, pos[0].position, (WORD)count);
    return g_origSetMultiple(id, pos, count, type);
}

static int __cdecl HookSetEnv(DWORD id, const AkEnvironmentValue* values, DWORD count) {
    AcquireSRWLockExclusive(&g_lock);
    if (Obj* o = Slot(id, true)) {
        o->envCount = 0;
        for (DWORD i = 0; values && i < count && o->envCount < kMaxEnv; i++)
            if (values[i].value > 0.0f) o->env[o->envCount++] = values[i];
    }
    ReleaseSRWLockExclusive(&g_lock);
    return g_origSetEnv(id, values, count);
}

static DWORD __cdecl HookGetId(const char* text) {
    DWORD id = g_origGetId(text);
    if (!id) return id;
    DWORD h = (id * 2654435761u) & (kNameTableSize - 1);
    AcquireSRWLockShared(&g_lock);
    bool known = false;
    for (int i = 0; i < 8; i++) {
        const Name& n = g_names[(h + i) & (kNameTableSize - 1)];
        if (n.id == id) known = true;
        if (n.id == id || !n.id) break;
    }
    ReleaseSRWLockShared(&g_lock);
    if (known) return id;
    char copy[40];
    CopyName(text, copy, sizeof(copy));
    if (!copy[0]) return id;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < 8; i++) {  // a short probe: a full table just stops learning names
        Name& n = g_names[(h + i) & (kNameTableSize - 1)];
        if (n.id == id) break;
        if (!n.id) {
            n.id = id;
            strcpy_s(n.text, copy);
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return id;
}

const char* SpatialName(unsigned long id) {
    DWORD h = (id * 2654435761u) & (kNameTableSize - 1);
    const char* found = nullptr;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < 8; i++) {
        const Name& n = g_names[(h + i) & (kNameTableSize - 1)];
        if (n.id == id) found = n.text;  // entries are never removed, so the text stays valid
        if (n.id == id || !n.id) break;
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

static bool Hook(DWORD rva, const BYTE* prolog, size_t n, void* detour, void** original, const char* name) {
    BYTE* target = g_base + rva;
    if (memcmp(target, prolog, n) != 0) {
        OverrideLog("!! spatial: %s does not look as expected, not hooked", name);
        return false;
    }
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
        OverrideLog("!! spatial: hooking %s failed", name);
        return false;
    }
    return true;
}

static void Install() {
    g_base = (BYTE*)GetModuleHandleA(nullptr);
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* file = strrchr(exe, '\\');
    if (_stricmp(file ? file + 1 : exe, "RaccoonCity.exe") != 0) return;  // self-test: no game to look into
    MH_STATUS init = MH_Initialize();  // tweaks.cpp may have done it already
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        OverrideLog("!! spatial: MinHook init failed");
        return;
    }
    // Names first: object names and IDs come in as soon as the hooks are live.
    int ok = Hook(kRvaGetIdFromString, kPrologGetId, sizeof(kPrologGetId), HookGetId, (void**)&g_origGetId, "GetIDFromString") +
             Hook(kRvaRegisterObj, kPrologObj, sizeof(kPrologObj), HookRegisterObj, (void**)&g_origRegisterObj, "RegisterGameObj") +
             Hook(kRvaUnregisterObj, kPrologObj, sizeof(kPrologObj), HookUnregisterObj, (void**)&g_origUnregisterObj, "UnregisterGameObj") +
             Hook(kRvaSetPosition, kPrologPosition, sizeof(kPrologPosition), HookSetPosition, (void**)&g_origSetPosition, "SetPosition") +
             Hook(kRvaSetMultiple, kPrologMultiple, sizeof(kPrologMultiple), HookSetMultiple, (void**)&g_origSetMultiple, "SetMultiplePositions") +
             Hook(kRvaSetEnvValues, kPrologEnv, sizeof(kPrologEnv), HookSetEnv, (void**)&g_origSetEnv, "SetGameObjectEnvironmentsValues");
    if (!memcmp(g_base + kRvaGetListenerPosition, kPrologListener, sizeof(kPrologListener)))
        g_getListener = (GetListenerFn)(g_base + kRvaGetListenerPosition);
    OverrideLog("spatial: %d of 6 Wwise hooks in, listener query %s", ok, g_getListener ? "ok" : "missing");
}

// ---- queries (any thread) ----

static float Dist(const AkVector& a, const AkVector& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

bool SpatialListener(SpatialFrame* out) {
    if (!g_installed || !g_getListener) return false;
    AkListenerPosition lp = {};
    if (g_getListener(0, &lp) != 1) return false;  // AK_Success
    if (lp.position.x == 0.0f && lp.position.y == 0.0f && lp.position.z == 0.0f) return false;  // menus: no camera yet
    memcpy(out->pos, &lp.position, 12);
    memcpy(out->front, &lp.front, 12);
    memcpy(out->top, &lp.top, 12);
    return true;
}

bool SpatialLocalCharacter(SpatialPoint* out) {
    SpatialFrame listener;
    if (!SpatialListener(&listener)) return false;
    AkVector lp = {listener.pos[0], listener.pos[1], listener.pos[2]};
    DWORD now = GetTickCount();
    const Obj* best = nullptr;
    float bestDist = 1e9f;
    AcquireSRWLockShared(&g_lock);
    for (const Obj& o : g_objs) {
        if (!o.id || !o.human || !o.moved || now - o.moved > kFreshMs) continue;
        float d = Dist(o.pos, lp);
        if (d < bestDist) {
            bestDist = d;
            best = &o;
        }
    }
    if (best) {
        memcpy(out->pos, &best->pos, 12);
        out->envCount = 0;
        for (int i = 0; i < best->envCount && out->envCount < kSpatialEnvs; i++) {
            out->envId[out->envCount] = best->env[i].envId;
            out->envValue[out->envCount++] = best->env[i].value;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return best != nullptr;
}

// ---- diag report (pump thread) ----

static void Report() {
    SpatialFrame lf;
    bool haveListener = SpatialListener(&lf);
    AkVector lpos = {lf.pos[0], lf.pos[1], lf.pos[2]};

    struct Near { DWORD id; float d; char name[32]; WORD posCount; int envCount; AkEnvironmentValue env[kMaxEnv]; bool envChanged; };
    Near nearest[5];
    int nearCount = 0, alive = 0;
    bool anyEnvChange = false;
    DWORD now = GetTickCount();
    AcquireSRWLockExclusive(&g_lock);
    for (Obj& o : g_objs) {
        if (!o.id || !o.moved || now - o.moved > kFreshMs) continue;
        alive++;
        if (!haveListener) continue;
        float d = Dist(o.pos, lpos);
        int at = nearCount < 5 ? nearCount++ : 5;
        while (at > 0 && nearest[at - 1].d > d) {
            if (at < 5) nearest[at] = nearest[at - 1];
            at--;
        }
        if (at < 5) {
            Near& nr = nearest[at];
            nr.id = o.id;
            nr.d = d;
            strcpy_s(nr.name, o.name);
            nr.posCount = o.posCount;
            nr.envCount = o.envCount;
            memcpy(nr.env, o.env, sizeof(nr.env));
            DWORD sum = 0;
            for (int i = 0; i < o.envCount; i++) sum += o.env[i].envId * 31 + (DWORD)(o.env[i].value * 100);
            nr.envChanged = sum != o.envSeen;
            o.envSeen = sum;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (int i = 0; i < nearCount; i++) anyEnvChange |= nearest[i].envChanged;

    // The scene only when the camera moved, an environment changed nearby, or now and then.
    static AkVector lastPos;
    static DWORD lastScene;
    bool moved = haveListener && Dist(lpos, lastPos) > 1.0f;
    if (!haveListener || !(moved || anyEnvChange || now - lastScene >= kQuietReportMs)) return;
    lastPos = lpos;
    lastScene = now;

    SpatialPoint me;
    bool haveMe = SpatialLocalCharacter(&me);
    OverrideLog("spatial: listener at %.2f %.2f %.2f, front %.2f %.2f %.2f; %d live objects, %ld positions set; me %s",
                lpos.x, lpos.y, lpos.z, lf.front[0], lf.front[1], lf.front[2], alive, g_setPositions,
                haveMe ? "found" : "not found");
    for (int i = 0; i < nearCount; i++) {
        const Near& nr = nearest[i];
        // Truncating, never the _s overflow handler: that ends the game on the spot (four long zone names did).
        char env[kMaxEnv * 64] = "";
        for (int e = 0; e < nr.envCount; e++) {
            char item[64];
            const char* name = SpatialName(nr.env[e].envId);
            _snprintf_s(item, _TRUNCATE, " %08lX%s%s%s=%.2f", nr.env[e].envId, name ? " '" : "", name ? name : "",
                        name ? "'" : "", nr.env[e].value);
            strncat_s(env, item, _TRUNCATE);
        }
        OverrideLog("spatial:   obj %08lX '%s' %.1f m, %u pos%s%s%s", nr.id, nr.name, nr.d, nr.posCount,
                    nr.envCount ? ", env" : "", env, nr.envChanged ? " (changed)" : "");
    }
}

void SpatialTick() {
    if (!g_installed) {
        // Hooks only when something uses them: spatial voice, or the diag report.
        bool wanted = OrcsteamDiag() || (OrcsteamIniInt("voice", 1) != 0 && OrcsteamIniInt("voice_spatial", 1) != 0);
        if (wanted) Install();
        InterlockedExchange(&g_installed, wanted ? 1 : 2);
    }
    if (g_installed != 1 || !OrcsteamDiag()) return;
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < kReportEveryMs) return;
    last = now;
    Report();
}
