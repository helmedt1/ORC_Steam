// orcsteam: a stand-alone xlive.dll for Resident Evil: Operation Raccoon City that replaces
// Games for Windows Live with Steam (identity, lobbies, invites, P2P traffic).
//
// Every ordinal the game imports goes through a tiny stub (push ordinal; jmp CommonEntry) to our
// implementation. The stub costs nothing by default; with tracing on (ORCSTEAM_TRACE=1 or trace=1
// in orcsteam.ini next to the DLL) it also logs each call with its first stack arguments and result.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "names.h"
#include "overrides.h"

static const char kVersion[] = "1.4";
static const LONG kVerboseCalls = 40;  // traced lines per ordinal, then only counted
static const int kMaxDepth = 64;       // per-thread nesting of traced calls
static const DWORD kSummaryEveryMs = 15000;
static const unsigned kMaxOrdinal = 6000;
static const LONG kMaxCrashLogs = 3;

struct OrdInfo {
    unsigned ord;
    const char* name;
    void* target;
    volatile LONG calls;
};

#define X(n) { n, nullptr, nullptr, 0 },
static OrdInfo g_ords[] = {
#include "ordinals.inc"
};
#undef X

static OrdInfo* g_byOrd[kMaxOrdinal];
static bool g_trace;
static CRITICAL_SECTION g_logCs;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static LARGE_INTEGER g_freq, g_t0;
static volatile LONG g_lastSummary;
static volatile LONG g_lastOrd;  // most recent ordinal entered on any thread, for crash reports
static volatile LONG g_crashLogs;

struct Frame { DWORD ret; unsigned ord; bool verbose; LARGE_INTEGER t; };
struct ThreadStack { int depth; Frame f[kMaxDepth]; };
static __declspec(thread) ThreadStack t_stack;

extern "C" void ReturnThunk();
extern "C" void MissingTarget();

// ---- log ----

static double MsSince(const LARGE_INTEGER& t) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - t.QuadPart) * 1000.0 / g_freq.QuadPart;
}

static void Log(const char* fmt, ...) {
    char buf[1024];
    int n = sprintf_s(buf, "[%10.3f] [%5lu] ", MsSince(g_t0) / 1000.0, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    n += (m < 0) ? 0 : (m > (int)sizeof(buf) - n - 3 ? (int)sizeof(buf) - n - 3 : m);
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_logCs);
    DWORD written;
    if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, buf, n, &written, nullptr);
    LeaveCriticalSection(&g_logCs);
}

void OverrideLog(const char* fmt, ...) {
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("   %s", buf);
}

static void ModuleDir(HMODULE mod, char* out, size_t cap) {
    GetModuleFileNameA(mod, out, (DWORD)cap);
    char* slash = strrchr(out, '\\');
    if (slash) slash[1] = 0;
}

// ---- crash reports ----

static const char* ModuleOf(void* addr, char* name, size_t cap, DWORD* offset) {
    HMODULE mod = nullptr;
    *offset = 0;
    strcpy_s(name, cap, "?");
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod)) {
        char path[MAX_PATH];
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* slash = strrchr(path, '\\');
        strcpy_s(name, cap, slash ? slash + 1 : path);
        *offset = (DWORD)((BYTE*)addr - (BYTE*)mod);
    }
    return name;
}

// Logs fatal-looking exceptions (first chance, so the game's own handlers still run afterwards).
// Stack-cookie failures (__fastfail) bypass this; a crash with no entry here usually means one.
static LONG CALLBACK CrashLogger(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xC0000409)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_crashLogs) > kMaxCrashLogs) return EXCEPTION_CONTINUE_SEARCH;

    const CONTEXT* c = ep->ContextRecord;
    char mod[64];
    DWORD off;
    ModuleOf(ep->ExceptionRecord->ExceptionAddress, mod, sizeof(mod), &off);
    unsigned last = (unsigned)g_lastOrd;
    Log("!!!! EXCEPTION (first chance, may still be handled) %08lX at %p (%s+%lX), last xlive call #%u %s", code,
        ep->ExceptionRecord->ExceptionAddress, mod, off, last,
        last < kMaxOrdinal && g_byOrd[last] ? g_byOrd[last]->name : "-");
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        Log("!!!! %s address %08lX", ep->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from",
            (DWORD)ep->ExceptionRecord->ExceptionInformation[1]);
    Log("!!!! eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX ebp %08lX esp %08lX", c->Eax, c->Ebx, c->Ecx,
        c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    const DWORD* sp = (const DWORD*)c->Esp;
    for (int i = 0; i < 24; i++) {
        DWORD v;
        if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, sizeof(v), nullptr)) break;
        char vm[64];
        DWORD voff;
        ModuleOf((void*)v, vm, sizeof(vm), &voff);
        if (strcmp(vm, "?")) Log("!!!!   [esp+%02X] %08lX  %s+%lX", i * 4, v, vm, voff);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- dispatch ----

static void WriteSummary(const char* reason) {
    Log("==== call counts (%s) ====", reason);
    for (OrdInfo& oi : g_ords)
        if (oi.calls) Log("  #%-5u %-36s %ld", oi.ord, oi.name, oi.calls);
}

static void ApplyOverrides(const Override* table, int count) {
    for (int i = 0; i < count; i++) {
        OrdInfo* oi = table[i].ord < kMaxOrdinal ? g_byOrd[table[i].ord] : nullptr;
        if (oi) oi->target = table[i].fn;
    }
}

// frame[0] = ordinal pushed by the stub, frame[1] = caller's return address, frame[2..] = args.
extern "C" void* __cdecl OnEnter(DWORD* frame) {
    unsigned ord = frame[0];
    OrdInfo* oi = g_byOrd[ord];
    g_lastOrd = (LONG)ord;
    if (!g_trace) return oi->target;

    DWORD err = GetLastError();
    LONG n = InterlockedIncrement(&oi->calls);
    bool verbose = n <= kVerboseCalls;
    if (verbose) {
        DWORD* a = frame + 2;
        Log("> #%u %s  from %08lX  args %08lX %08lX %08lX %08lX %08lX %08lX%s", ord, oi->name, frame[1], a[0], a[1],
            a[2], a[3], a[4], a[5], n == kVerboseCalls ? "  (further calls counted only)" : "");
    }
    ThreadStack& ts = t_stack;
    if (ts.depth < kMaxDepth) {
        Frame& f = ts.f[ts.depth++];
        f.ret = frame[1];
        f.ord = ord;
        f.verbose = verbose;
        QueryPerformanceCounter(&f.t);
        frame[1] = (DWORD)ReturnThunk;
    }
    LONG now = (LONG)GetTickCount();
    LONG last = g_lastSummary;
    if ((DWORD)(now - last) > kSummaryEveryMs && InterlockedCompareExchange(&g_lastSummary, now, last) == last)
        WriteSummary("periodic");
    SetLastError(err);
    return oi->target;
}

// slot = where the real return address must be written; slot[-1] = saved EAX (return value).
extern "C" void __cdecl OnLeave(DWORD* slot) {
    DWORD err = GetLastError();
    ThreadStack& ts = t_stack;
    Frame& f = ts.f[--ts.depth];
    if (f.verbose)
        Log("< #%u %s = %08lX  lastErr %lu  %.3f ms", f.ord, g_byOrd[f.ord]->name, slot[-1], err, MsSince(f.t));
    *slot = f.ret;
    SetLastError(err);
}

extern "C" __declspec(naked) void CommonEntry() {
    __asm {
        pushad
        lea eax, [esp + 32]
        push eax
        call OnEnter
        add esp, 4
        mov [esp + 28], eax  // becomes EAX after popad
        popad
        add esp, 4           // drop ordinal
        jmp eax
    }
}

extern "C" __declspec(naked) void ReturnThunk() {
    __asm {
        sub esp, 4           // slot for the real return address
        pushad
        lea ecx, [esp + 32]
        push ecx
        call OnLeave
        add esp, 4
        popad
        ret
    }
}

extern "C" __declspec(naked) void MissingTarget() {
    __asm {
        xor eax, eax
        ret
    }
}

#define X(n) extern "C" __declspec(naked) void Stub_##n() { __asm push n __asm jmp CommonEntry }
#include "ordinals.inc"
#undef X

// For the self-test: how many exported ordinals have no implementation.
extern "C" __declspec(dllexport) int __cdecl OrcsteamMissingCount() {
    int missing = 0;
    for (OrdInfo& oi : g_ords)
        if (oi.target == (void*)MissingTarget) missing++;
    return missing;
}

// For tools/audit_impl.ps1: RVA of the implementation behind an ordinal (0 if not exported).
extern "C" __declspec(dllexport) DWORD __cdecl OrcsteamTargetRva(unsigned ord) {
    if (ord >= kMaxOrdinal || !g_byOrd[ord]) return 0;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&OrcsteamTargetRva, &self);
    return (DWORD)((BYTE*)g_byOrd[ord]->target - (BYTE*)self);
}

// ---- settings: [orcsteam] section of orcsteam.ini next to the DLL (optional file) ----

static char g_iniPath[MAX_PATH];

int OrcsteamIniInt(const char* key, int fallback) {
    return GetPrivateProfileIntA("orcsteam", key, fallback, g_iniPath);
}

void OrcsteamIniString(const char* key, const char* fallback, char* out, unsigned long cap) {
    GetPrivateProfileStringA("orcsteam", key, fallback, out, cap, g_iniPath);
}

static bool g_diag;

bool OrcsteamDiag() { return g_diag; }

static bool Requested(const char* env, const char* key) {
    char value[8];
    if (GetEnvironmentVariableA(env, value, sizeof(value)) && value[0] == '1') return true;
    return OrcsteamIniInt(key, 0) == 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_logCs);
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_t0);
        g_lastSummary = (LONG)GetTickCount();
        ModuleDir(inst, g_iniPath, MAX_PATH);
        strcat_s(g_iniPath, "orcsteam.ini");
        g_trace = Requested("ORCSTEAM_TRACE", "trace");
        g_diag = Requested("ORCSTEAM_DIAG", "diag");

        for (OrdInfo& oi : g_ords) {
            oi.name = "?";
            for (const OrdName& on : kOrdNames)
                if (on.ord == oi.ord) oi.name = on.name;
            oi.target = (void*)MissingTarget;
            g_byOrd[oi.ord] = &oi;
        }
        ApplyOverrides(kOverrides, kOverrideCount);
        ApplyOverrides(kSessionOverrides, kSessionOverrideCount);
        ApplyOverrides(kNetOverrides, kNetOverrideCount);
        ApplyOverrides(kInviteOverrides, kInviteOverrideCount);
        ApplyOverrides(kCompatOverrides, kCompatOverrideCount);
        ApplyOverrides(kVoiceOverrides, kVoiceOverrideCount);

        char path[MAX_PATH];
        ModuleDir(inst, path, MAX_PATH);
        strcat_s(path, "orcsteam.log");
        g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        Log("orcsteam %s%s%s. exe: %s", kVersion, g_trace ? " (trace on)" : "", g_diag ? " (diag on)" : "", exe);
        for (OrdInfo& oi : g_ords)
            if (oi.target == (void*)MissingTarget) Log("!! no implementation for #%u %s", oi.ord, oi.name);
        AddVectoredExceptionHandler(1, CrashLogger);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_trace) WriteSummary("exit");
        Log("exit");
        if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    }
    return TRUE;
}
