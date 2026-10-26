// Steam access through the game's own steam_api.dll, plus a pump thread that
// completes async Steam calls (by polling, no CCallResult needed) and moves P2P packets.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "steam.h"
#include "overrides.h"

static const int kMaxAsync = 32;
static const DWORD kSessionsPumpMs = 250;

struct AsyncCall {
    SteamAPICall_t call;
    int callbackId;
    int size;
    AsyncDone done;
    void* ctx;
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static AsyncCall g_async[kMaxAsync];
static SteamApi g_api;
static volatile LONG g_ready;
static volatile LONG g_stop;
static HANDLE g_pump;

// Accept P2P sessions from anyone who talks to us: the game itself decides who may join.
class P2PRequestCallback : public CCallbackBase {
public:
    void Run(void* param) override {
        const P2PSessionRequest_t* req = (const P2PSessionRequest_t*)param;
        if (g_api.net) g_api.net->AcceptP2PSessionWithUser(req->m_steamIDRemote);
        OverrideLog("p2p: accepted session from %08lX", req->m_steamIDRemote.AccountId());
    }
    void Run(void* param, bool, SteamAPICall_t) override { Run(param); }
    int GetCallbackSizeBytes() override { return sizeof(P2PSessionRequest_t); }
};
static P2PRequestCallback g_p2pRequest;

// Callbacks that only log what Steam reports.
class LogCallback : public CCallbackBase {
public:
    LogCallback(void (*fn)(const void*), int size) : m_fn(fn), m_size(size) {}
    void Run(void* param) override { m_fn(param); }
    void Run(void* param, bool, SteamAPICall_t) override { Run(param); }
    int GetCallbackSizeBytes() override { return m_size; }
private:
    void (*m_fn)(const void*);
    int m_size;
};

// 1 not running app, 2 no rights to app, 3 destination not logged in, 4 timeout
static void OnP2PConnectFail(const void* param) {
    const P2PSessionConnectFail_t* f = (const P2PSessionConnectFail_t*)param;
    OverrideLog("!! p2p: session with %lu failed, error %u", f->m_steamIDRemote.AccountId(), f->m_eP2PSessionError);
}

static void OnLobbyChatUpdate(const void* param) {
    const LobbyChatUpdate_t* u = (const LobbyChatUpdate_t*)param;
    OverrideLog("lobby %016llX: account %lu state change %lX (by %lu)", u->m_ulSteamIDLobby,
                CSteamID(u->m_ulSteamIDUserChanged).AccountId(), u->m_rgfChatMemberStateChange,
                CSteamID(u->m_ulSteamIDMakingChange).AccountId());
}

static LogCallback g_p2pFail(OnP2PConnectFail, sizeof(P2PSessionConnectFail_t));
static LogCallback g_lobbyChatUpdate(OnLobbyChatUpdate, sizeof(LobbyChatUpdate_t));

static void PollAsync(const SteamApi& s) {
    AsyncCall ready[kMaxAsync];
    bool failedFlags[kMaxAsync];
    int n = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (AsyncCall& a : g_async) {
        if (!a.call) continue;
        bool failed = false;
        if (!s.utils->IsAPICallCompleted(a.call, &failed)) continue;
        ready[n] = a;
        failedFlags[n++] = failed;
        a.call = 0;
    }
    ReleaseSRWLockExclusive(&g_lock);

    for (int i = 0; i < n; i++) {
        BYTE buf[256] = {};
        bool failed = failedFlags[i];
        if (!failed && !s.utils->GetAPICallResult(ready[i].call, buf, ready[i].size, ready[i].callbackId, &failed))
            failed = true;
        ready[i].done(ready[i].ctx, buf, failed);
    }
}

static DWORD WINAPI PumpThread(LPVOID) {
    OverrideLog("steam pump started");
    DWORD lastSessions = 0;
    while (!g_stop) {
        PollAsync(g_api);
        NetPumpReceive(g_api);
        DWORD now = GetTickCount();
        if (now - lastSessions >= kSessionsPumpMs) {
            SessionsPump(g_api);
            InvitesPump(g_api);
            lastSessions = now;
        }
        Sleep(1);
    }
    OverrideLog("steam pump stopped");
    return 0;
}

const SteamApi* Steam() {
    if (g_ready) return g_stop ? nullptr : &g_api;
    HMODULE dll = GetModuleHandleA("steam_api.dll");
    if (!dll) return nullptr;
    typedef void* (__cdecl * Getter)();
    Getter user = (Getter)GetProcAddress(dll, "SteamUser");
    Getter friends = (Getter)GetProcAddress(dll, "SteamFriends");
    Getter mm = (Getter)GetProcAddress(dll, "SteamMatchmaking");
    Getter net = (Getter)GetProcAddress(dll, "SteamNetworking");
    Getter utils = (Getter)GetProcAddress(dll, "SteamUtils");
    if (!user || !friends || !mm || !net || !utils) return nullptr;
    SteamApi api = {(ISteamUser016*)user(), (ISteamFriends009*)friends(), (ISteamMatchmaking008*)mm(),
                    (ISteamNetworking005*)net(), (ISteamUtils005*)utils()};
    if (!api.user || !api.friends || !api.mm || !api.net || !api.utils) return nullptr;  // SteamAPI_Init not done yet

    bool first = false;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_ready) {
        g_api = api;
        g_api.net->AllowP2PPacketRelay(true);
        SteamRegisterCallback(&g_p2pRequest, P2PSessionRequest_t::k_iCallback);
        SteamRegisterCallback(&g_p2pFail, P2PSessionConnectFail_t::k_iCallback);
        SteamRegisterCallback(&g_lobbyChatUpdate, LobbyChatUpdate_t::k_iCallback);
        OverrideLog("steam ready: appid %lu", g_api.utils->GetAppID());
        g_pump = CreateThread(nullptr, 0, PumpThread, nullptr, 0, nullptr);
        InterlockedExchange(&g_ready, 1);
        first = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (first) InvitesOnSteamReady(g_api);
    return &g_api;
}

void SteamRegisterCallback(CCallbackBase* cb, int callbackId) {
    typedef void(__cdecl * RegisterCallback)(CCallbackBase*, int);
    static RegisterCallback reg =
        (RegisterCallback)GetProcAddress(GetModuleHandleA("steam_api.dll"), "SteamAPI_RegisterCallback");
    if (!reg) {
        OverrideLog("!! SteamAPI_RegisterCallback missing, callback %d not registered", callbackId);
        return;
    }
    cb->m_iCallback = callbackId;
    reg(cb, callbackId);
    OverrideLog("steam callback %d registered", callbackId);
}

void SteamAsync(SteamAPICall_t call, int callbackId, int resultSize, AsyncDone done, void* ctx) {
    if (!call) {
        done(ctx, nullptr, true);
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    for (AsyncCall& a : g_async)
        if (!a.call) {
            a = {call, callbackId, resultSize, done, ctx};
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
    ReleaseSRWLockExclusive(&g_lock);
    OverrideLog("!! async table full, dropping steam call");
    done(ctx, nullptr, true);
}

void SteamStopPump() {
    if (!g_pump) return;
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(g_pump, 2000);
    CloseHandle(g_pump);
    g_pump = nullptr;
}
