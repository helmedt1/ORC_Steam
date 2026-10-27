// Self-test for orcsteam's xlive.dll: return values, stack balance, the old stub's behaviour the game relies on,
// transport over loopback and, with "steam" as argument, lobbies, search and invites against a running Steam.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma comment(lib, "ws2_32.lib")
#include "steam.h"

typedef DWORD(WINAPI* Fn1)(DWORD);
typedef DWORD(WINAPI* Fn0)();
typedef DWORD(WINAPI* FnSigninInfo)(DWORD, DWORD, void*);

static DWORD CallChecked(FARPROC f, int nargs, bool& espOk) {
    DWORD before, after, r;
    __asm mov before, esp
    r = nargs ? ((Fn1)f)(0) : ((Fn0)f)();
    __asm mov after, esp
    espOk = before == after;
    return r;
}

static FARPROC Ord(HMODULE m, unsigned o) { return GetProcAddress(m, MAKEINTRESOURCEA(o)); }

// A datagram to our own virtual address must come back through the loopback injector,
// with the header stripped and the sender reported as the virtual address.
static int TestTransport(HMODULE proxy) {
    typedef DWORD(WINAPI * FnTitleAddr)(BYTE*);
    typedef int(WINAPI * FnXnToIn)(const BYTE*, const void*, IN_ADDR*);
    typedef SOCKET(WINAPI * FnCreate)(int, int, int);
    typedef int(WINAPI * FnBind)(SOCKET, const sockaddr*, int);
    typedef int(WINAPI * FnSendTo)(SOCKET, const char*, int, int, const sockaddr*, int);
    typedef int(WINAPI * FnRecvFrom)(SOCKET, char*, int, int, sockaddr*, int*);
    typedef int(WINAPI * FnClose)(SOCKET);

    BYTE xna[36] = {};
    ((FnTitleAddr)Ord(proxy, 73))(xna);
    IN_ADDR ina = {};
    int r = ((FnXnToIn)Ord(proxy, 57))(xna, nullptr, &ina);

    SOCKET s = ((FnCreate)Ord(proxy, 3))(AF_INET, SOCK_DGRAM, 254);  // VDP, like the game
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(31337);
    ((FnBind)Ord(proxy, 11))(s, (sockaddr*)&local, sizeof(local));
    DWORD timeout = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_addr = ina;
    to.sin_port = htons(31337);
    const char msg[] = "hello over steam";
    int sent = ((FnSendTo)Ord(proxy, 24))(s, msg, sizeof(msg), 0, (sockaddr*)&to, sizeof(to));

    char buf[256] = {};
    sockaddr_in from = {};
    int fromLen = sizeof(from);
    int got = ((FnRecvFrom)Ord(proxy, 20))(s, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
    ((FnClose)Ord(proxy, 4))(s);

    bool ok = r == 0 && (ina.S_un.S_un_b.s_b1 == 10) && sent == (int)sizeof(msg) && got == (int)sizeof(msg) &&
              !memcmp(buf, msg, sizeof(msg)) && from.sin_addr.s_addr == ina.s_addr && from.sin_port == htons(31337);
    printf("transport: virtual %u.%u.%u.%u, sent %d, got %d '%s' from %u.%u.%u.%u:%u -> %s\n", ina.S_un.S_un_b.s_b1,
           ina.S_un.S_un_b.s_b2, ina.S_un.S_un_b.s_b3, ina.S_un.S_un_b.s_b4, sent, got, buf,
           from.sin_addr.S_un.S_un_b.s_b1, from.sin_addr.S_un.S_un_b.s_b2, from.sin_addr.S_un.S_un_b.s_b3,
           from.sin_addr.S_un.S_un_b.s_b4, ntohs(from.sin_port), ok ? "ok" : "BAD");
    return ok ? 0 : 1;
}

typedef bool(__cdecl* SteamInitFn)();
typedef void(__cdecl* SteamVoidFn)();
static SteamVoidFn steamRun, steamShutdown;

static void PumpFor(DWORD ms) {
    for (DWORD t = GetTickCount(); GetTickCount() - t < ms;) {
        if (steamRun) steamRun();
        Sleep(10);
    }
}

// Invite / "Join game": a lobby made with raw Steam calls stands in for a friend's game; the join request is
// injected through ORCSTEAM_TEST_JOIN. The game-facing side must see XN_LIVE_INVITE_ACCEPTED, then the
// host's session info from XInviteGetAcceptedInfo, then a client session that enters the same lobby.
static int TestInvite(HMODULE proxy, HMODULE api) {
    typedef void* (__cdecl * Getter)();
    ISteamMatchmaking008* mm = (ISteamMatchmaking008*)((Getter)GetProcAddress(api, "SteamMatchmaking"))();
    ISteamUtils005* utils = (ISteamUtils005*)((Getter)GetProcAddress(api, "SteamUtils"))();
    int fails = 0;

    typedef HANDLE(WINAPI * FnListener)(ULONGLONG);
    typedef BOOL(WINAPI * FnNext)(HANDLE, DWORD, DWORD*, ULONG_PTR*);
    HANDLE listener = ((FnListener)Ord(proxy, 5270))(3);  // system | live, like the game's

    SteamAPICall_t call = mm->CreateLobby(kLobbyPublic, 4);
    LobbyCreated_t created = {};
    bool failed = true;
    for (int i = 0; i < 500 && !utils->IsAPICallCompleted(call, &failed); i++) PumpFor(10);
    utils->GetAPICallResult(call, &created, sizeof(created), LobbyCreated_t::k_iCallback, &failed);
    CSteamID lobby(created.m_ulSteamIDLobby);
    BYTE info[60] = {};
    info[0] = 0x8A;  // kid: online peer
    info[1] = 0x42;
    DWORD fakeHost = 1234567;
    memcpy(info + 8 + 16, &fakeHost, 4);  // hostAddress.abOnline = host account id
    char hex[121] = {};
    for (int i = 0; i < 60; i++) sprintf_s(hex + i * 2, 3, "%02X", info[i]);
    mm->SetLobbyData(lobby, "orcsteam", "1");
    mm->SetLobbyData(lobby, "info", hex);
    printf("invite: stand-in lobby %016llX (create %s)\n", lobby.v, failed ? "FAILED" : "ok");

    char id[32];
    sprintf_s(id, "%llu", lobby.v);
    SetEnvironmentVariableA("ORCSTEAM_TEST_JOIN", id);
    DWORD note = 0;
    ULONG_PTR param = 99;
    for (int i = 0; i < 500 && !note; i++) {
        PumpFor(10);
        DWORD got = 0;
        if (((FnNext)Ord(proxy, 651))(listener, 0, &got, &param) && got == 0x02000002) note = got;
    }
    printf("invite: notification %08lX param %lu\n", note, (DWORD)param);
    if (note != 0x02000002 || param != 0) fails++;

    // Same layout as the game: 84 bytes, followed by a guard the DLL must not touch.
#pragma pack(push, 4)
    struct { unsigned long long invitee, inviter; DWORD title; BYTE host[60]; BOOL fromGame; DWORD guard; } inv = {};
#pragma pack(pop)
    inv.guard = 0xC00C1E5;
    typedef DWORD(WINAPI * FnAccepted)(DWORD, void*);
    DWORD r = ((FnAccepted)Ord(proxy, 5315))(0, &inv);
    printf("invite: guard after the 84-byte info %s\n", inv.guard == 0xC00C1E5 ? "intact" : "OVERWRITTEN");
    if (inv.guard != 0xC00C1E5) fails++;
    printf("invite: accepted info r=%lu inviter %016llX title %08lX kid %02X%02X\n", r, inv.inviter, inv.title,
           inv.host[0], inv.host[1]);
    if (r != 0 || memcmp(inv.host, info, 60) || inv.title != 0x43430FA1 || (inv.inviter & 0xFFFFFFFF) != fakeHost) fails++;

    typedef DWORD(WINAPI * FnCreate)(DWORD, DWORD, DWORD, DWORD, void*, void*, void*, HANDLE*);
    unsigned long long nonce = 0;
    HANDLE client = nullptr;
    ((FnCreate)Ord(proxy, 5300))(0x2C, 0, 4, 0, &nonce, inv.host, nullptr, &client);
    PumpFor(1500);

    typedef DWORD(WINAPI * FnFriends)(DWORD, DWORD, DWORD, DWORD*, HANDLE*);
    typedef DWORD(WINAPI * FnEnum)(HANDLE, void*, DWORD, DWORD*, void*);
    DWORD cb = 0, items = 0;
    HANDLE fe = nullptr;
    r = ((FnFriends)Ord(proxy, 5312))(0, 0, 20, &cb, &fe);
    BYTE* buf = (BYTE*)calloc(1, cb ? cb : 1);
    DWORD r2 = ((FnEnum)Ord(proxy, 5256))(fe, buf, cb, &items, nullptr);
    printf("invite: friends enumerator r=%lu cb=%lu, enumerate r=%lu items=%lu\n", r, cb, r2, items);
    if (r != 0 || cb != 20 * 0xC4 || (r2 != 0 && r2 != ERROR_NO_MORE_FILES)) fails++;

    typedef BOOL(WINAPI * FnCloseH)(HANDLE);
    ((FnCloseH)Ord(proxy, 5251))(fe);
    ((FnCloseH)Ord(proxy, 5251))(client);
    mm->LeaveLobby(lobby);
    PumpFor(300);
    return fails;
}

// Real Steam: host a matchmaking session (lobby must appear), then search (own lobby is skipped).
static int TestSteam(HMODULE proxy) {
    HMODULE api = LoadLibraryA("steam_api.dll");
    SteamInitFn init = api ? (SteamInitFn)GetProcAddress(api, "SteamAPI_Init") : nullptr;
    if (!init || !init()) { printf("steam: SteamAPI_Init failed (is Steam running?)\n"); return 1; }
    steamRun = (SteamVoidFn)GetProcAddress(api, "SteamAPI_RunCallbacks");
    steamShutdown = (SteamVoidFn)GetProcAddress(api, "SteamAPI_Shutdown");

    struct { unsigned long long xuid; DWORD flags, state, guest, sponsor; char name[16]; } info = {};
    ((FnSigninInfo)Ord(proxy, 5267))(0, 0, &info);
    printf("steam: signed in as '%s', xuid %016llX\n", info.name, info.xuid);
    int nameFails = 0;
    for (const char* p = info.name; *p; p++)
        if (*p < 0x20 || *p > 0x7E) nameFails = 1;  // must be plain ASCII for the game's font

    typedef void(WINAPI * FnCtx)(DWORD, DWORD, DWORD);
    ((FnCtx)Ord(proxy, 5277))(0, 0x800A, 1);
    ((FnCtx)Ord(proxy, 5277))(0, 0x800B, 2);

    typedef DWORD(WINAPI * FnCreate)(DWORD, DWORD, DWORD, DWORD, void*, void*, void*, HANDLE*);
    BYTE sinfo[60] = {};
    unsigned long long nonce = 0;
    HANDLE session = nullptr;
    ((FnCreate)Ord(proxy, 5300))(0x2D, 0, 4, 0, &nonce, sinfo, nullptr, &session);
    PumpFor(4000);

    // Rich presence: hosting a matchmaking session sets a player group (and no status text).
    typedef void* (__cdecl * Getter)();
    ISteamFriends009* friends = (ISteamFriends009*)((Getter)GetProcAddress(api, "SteamFriends"))();
    ISteamUser016* user = (ISteamUser016*)((Getter)GetProcAddress(api, "SteamUser"))();
    CSteamID me = user->GetSteamID();
    const char* status = friends->GetFriendRichPresence(me, "status");
    const char* group = friends->GetFriendRichPresence(me, "steam_player_group");
    printf("steam: presence status '%s', group '%s'\n", status, group);
    if (status[0] || !group[0]) nameFails++;  // counted with the other early checks

    struct { ULONG_PTR low, high, ctx; HANDLE ev; void* routine; DWORD_PTR cctx; DWORD ext; } ov = {};
    ov.ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    typedef DWORD(WINAPI * FnSearch)(DWORD, DWORD, DWORD, WORD, WORD, void*, void*, DWORD*, void*, void*);
    DWORD cb = 0;
    ((FnSearch)Ord(proxy, 5321))(0, 0, 10, 0, 0, nullptr, nullptr, &cb, nullptr, nullptr);
    BYTE* results = (BYTE*)calloc(1, cb);
    SetEnvironmentVariableA("ORCSTEAM_SEARCH_SELF", "1");  // see our own lobby like a client would
    DWORD r = ((FnSearch)Ord(proxy, 5321))(0, 0, 10, 0, 0, nullptr, nullptr, &cb, results, &ov);
    DWORD wait = WaitForSingleObject(ov.ev, 10000);
    SetEnvironmentVariableA("ORCSTEAM_SEARCH_SELF", nullptr);
    DWORD count = *(DWORD*)results;
    printf("steam: search r=%lu, completed=%d, status=%lu, results=%lu\n", r, wait == WAIT_OBJECT_0, (DWORD)ov.low, count);
    int fails = nameFails + ((r != ERROR_IO_PENDING || wait != WAIT_OBJECT_0 || ov.low != 0 || count < 1) ? 1 : 0);

    // Walk the results exactly like RaccoonCity.exe does (offsets from its code at 0xBF2D97..0xBF2DAB).
    BYTE* res = *(BYTE**)(results + 4);
    for (DWORD i = 0; i < count; i++, res += 0x5C) {
        DWORD cProps = *(DWORD*)(res + 0x4C);
        BYTE* prop = *(BYTE**)(res + 0x54);
        const wchar_t* host = nullptr;
        for (DWORD p = 0; p < cProps; p++, prop += 0x18)
            if (*(DWORD*)prop == 0x40008109) host = *(const wchar_t**)(prop + 0x14);
        char narrow[64] = {};
        if (host) WideCharToMultiByte(CP_UTF8, 0, host, -1, narrow, sizeof(narrow), nullptr, nullptr);
        printf("steam: result %lu: %lu properties, host name property %s '%s'\n", i, cProps, host ? "present" : "MISSING", narrow);
        if (!host || !host[0]) fails++;
    }

    typedef BOOL(WINAPI * FnCloseH)(HANDLE);
    ((FnCloseH)Ord(proxy, 5251))(session);
    PumpFor(500);
    group = friends->GetFriendRichPresence(me, "steam_player_group");
    printf("steam: player group after close '%s'\n", group);
    if (group[0]) fails++;

    // lobby=friends: the hosted lobby must not be listed by a search (it is friends/invite only).
    WritePrivateProfileStringA("orcsteam", "lobby", "friends", ".\\orcsteam.ini");
    HANDLE privateSession = nullptr;
    ((FnCreate)Ord(proxy, 5300))(0x2D, 0, 4, 0, &nonce, sinfo, nullptr, &privateSession);
    PumpFor(3000);
    memset(&ov, 0, sizeof(ov));
    ov.ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    memset(results, 0, cb);
    SetEnvironmentVariableA("ORCSTEAM_SEARCH_SELF", "1");
    ((FnSearch)Ord(proxy, 5321))(0, 0, 10, 0, 0, nullptr, nullptr, &cb, results, &ov);
    WaitForSingleObject(ov.ev, 10000);
    SetEnvironmentVariableA("ORCSTEAM_SEARCH_SELF", nullptr);
    DWORD mine = 0;
    BYTE* r2 = *(BYTE**)(results + 4);
    for (DWORD i = 0; r2 && i < *(DWORD*)results; i++)
        if (!memcmp(r2 + i * 0x5C, sinfo, 8)) mine++;  // our session id
    printf("steam: friends-only lobby found by search %lu time(s)\n", mine);
    if (mine) fails++;
    ((FnCloseH)Ord(proxy, 5251))(privateSession);
    DeleteFileA(".\\orcsteam.ini");
    PumpFor(500);

    fails += TestInvite(proxy, api);  // api: the steam_api.dll module loaded above

    typedef DWORD(WINAPI * FnUninit)();
    ((FnUninit)Ord(proxy, 5003))();
    return fails;
}

// Behaviour the old stub had and the game relies on: saves, profile defaults, sign-in notification, voice.
static int TestCompat(HMODULE proxy) {
    int fails = 0;

    // Saves: protect/unprotect copy bytes as-is; a short buffer gets the size and 0x8007007A.
    typedef DWORD(WINAPI * FnProtect)(const BYTE*, DWORD, BYTE*, DWORD*, HANDLE);
    const BYTE save[] = "savegame-bytes";
    BYTE out[64] = {};
    DWORD cb = 4;
    DWORD r1 = ((FnProtect)Ord(proxy, 5034))(save, sizeof(save), out, &cb, nullptr);
    DWORD cbShort = cb;
    cb = sizeof(out);
    DWORD r2 = ((FnProtect)Ord(proxy, 5035))(save, sizeof(save), out, &cb, nullptr);
    bool saveOk = r1 == 0x8007007A && cbShort == sizeof(save) && r2 == 0 && cb == sizeof(save) && !memcmp(out, save, sizeof(save));
    printf("compat: save data short=%08lX/%lu copy=%lu/%lu -> %s\n", r1, cbShort, r2, cb, saveOk ? "ok" : "BAD");
    if (!saveOk) fails++;

    // Profile: int32 default, title-specific blob has no value, layout 8 + 0x28 per setting.
    typedef DWORD(WINAPI * FnProfile)(DWORD, DWORD, DWORD, const DWORD*, DWORD*, BYTE*, void*);
    const DWORD ids[] = {0x10040002, 0x63E83FFF, 0x40520040};  // int32, title-specific 1000-byte blob, 0x52-byte string
    DWORD need = 0;
    DWORD p1 = ((FnProfile)Ord(proxy, 5331))(0, 0, 3, ids, &need, nullptr, nullptr);
    BYTE* prof = (BYTE*)calloc(1, need);
    DWORD p2 = ((FnProfile)Ord(proxy, 5331))(0, 0, 3, ids, &need, prof, nullptr);
    BYTE* s = prof + 8;
    // The stub reserved room for every string/blob id, title-specific ones included (1000 bytes here).
    bool profOk = p1 == ERROR_INSUFFICIENT_BUFFER && need == 8 + 3 * 0x28 + 1000 + 0x52 && p2 == 0 && *(DWORD*)prof == 3 &&
                  *(DWORD*)(s + 0) == 1 && s[0x18] == 1 &&                  // int32: default
                  *(DWORD*)(s + 0x28) == 0 && s[0x28 + 0x18] == 0xFF &&     // blob: no value
                  s[0x50 + 0x18] == 4 && *(DWORD*)(s + 0x50 + 0x20) == 2;  // string: empty
    printf("compat: profile need=%lu -> %s\n", need, profOk ? "ok" : "BAD");
    if (!profOk) fails++;

    // Sign-in notification: posted by XLiveInitializeEx, taken once, then the queue is empty.
    typedef HANDLE(WINAPI * FnListener)(ULONGLONG);
    typedef BOOL(WINAPI * FnNext)(HANDLE, DWORD, DWORD*, ULONG_PTR*);
    typedef DWORD(WINAPI * FnInit)(const void*, DWORD);
    HANDLE l = ((FnListener)Ord(proxy, 5270))(1);
    DWORD id = 0;
    ULONG_PTR param = 0;
    BOOL before = ((FnNext)Ord(proxy, 651))(l, 0, &id, &param);
    ((FnInit)Ord(proxy, 5297))(nullptr, 0);
    bool signaled = WaitForSingleObject(l, 0) == WAIT_OBJECT_0;
    BOOL got = ((FnNext)Ord(proxy, 651))(l, 0, &id, &param);
    DWORD gotId = id;
    ULONG_PTR gotParam = param;
    BOOL again = ((FnNext)Ord(proxy, 651))(l, 0, &id, &param);
    bool noteOk = !before && signaled && got && gotId == 0x0A && gotParam == 1 && !again;
    printf("compat: signin notification %08lX param %lu -> %s\n", gotId, (DWORD)gotParam, noteOk ? "ok" : "BAD");
    if (!noteOk) fails++;

    // Voice engine: a COM-style object; talkers tracked, no chat data without a microphone, incoming data
    // marks the talker as talking unless the game muted it (playback priority NEVER).
    typedef LONG(WINAPI * FnHv)(const void*, HANDLE*, void***);
    void** engine = nullptr;
    LONG hr = ((FnHv)Ord(proxy, 5008))(nullptr, nullptr, &engine);
    typedef LONG(__stdcall * FnRegRemote)(void*, unsigned long long, void*, void*, void*);
    typedef LONG(__stdcall * FnGetTalkers)(void*, DWORD*, unsigned long long*);
    typedef LONG(__stdcall * FnChat)(void*, DWORD, BYTE*, DWORD*, DWORD*);
    typedef ULONG(__stdcall * FnRelease)(void*);
    typedef BOOL(__stdcall * FnHeadset)(void*, DWORD);
    typedef BOOL(__stdcall * FnRemoteTalking)(void*, unsigned long long);
    typedef LONG(__stdcall * FnPriority)(void*, unsigned long long, DWORD, DWORD);
    typedef LONG(__stdcall * FnSubmit)(void*, unsigned long long, const BYTE*, DWORD*);
    void** vt = engine ? (void**)*engine : nullptr;
    bool voiceOk = false;
    if (vt) {
        const unsigned long long a = 0x0009000012345678ull, b = 0x0009000012345679ull;
        ((FnRegRemote)vt[10])(engine, a, nullptr, nullptr, nullptr);
        ((FnRegRemote)vt[10])(engine, b, nullptr, nullptr, nullptr);
        DWORD count = 0, sz = 9, packets = 9;
        unsigned long long talkers[4] = {};
        ((FnGetTalkers)vt[12])(engine, &count, talkers);
        LONG chat = ((FnChat)vt[17])(engine, 0, nullptr, &sz, &packets);
        BOOL headset = ((FnHeadset)vt[13])(engine, 0);
        ((FnPriority)vt[18])(engine, b, 0, 0xFFFFFFFF);  // mute b
        const BYTE blob[] = {0x56, 1, 3, 0, 1, 2, 3};    // magic, version, one 3-byte chunk
        DWORD blobSize = sizeof(blob);
        ((FnSubmit)vt[19])(engine, a, blob, &blobSize);
        blobSize = sizeof(blob);
        ((FnSubmit)vt[19])(engine, b, blob, &blobSize);
        BOOL talkingA = ((FnRemoteTalking)vt[15])(engine, a), talkingB = ((FnRemoteTalking)vt[15])(engine, b);
        ULONG refs = ((FnRelease)vt[1])(engine);
        printf("compat: voice talkers %lu, chat %08lX, headset %d, talking %d/%d (b muted), refs %lu\n", count,
               (DWORD)chat, headset, talkingA, talkingB, refs);
        voiceOk = hr == 0 && count == 2 && talkers[0] == a && chat == (LONG)0x8000000A && !sz && !packets && headset &&
                  talkingA && !talkingB && refs == 0;
    }
    printf("compat: voice engine -> %s\n", voiceOk ? "ok" : "BAD");
    if (!voiceOk) fails++;
    return fails;
}

int main(int argc, char** argv) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    HMODULE proxy = LoadLibraryA("xlive.dll");
    if (!proxy) { printf("load failed %lu\n", GetLastError()); return 1; }
    int fails = 0;

    if (argc > 1 && !strcmp(argv[1], "targets")) {  // ordinal -> implementation RVA, for tools/audit_impl.ps1
        typedef DWORD(__cdecl * FnRva)(unsigned);
        FnRva rva = (FnRva)GetProcAddress(proxy, "OrcsteamTargetRva");
        for (unsigned o = 1; o < 6000; o++)
            if (DWORD v = rva(o)) printf("%u %08lX\n", o, v);
        return 0;
    }

    typedef int(__cdecl * FnMissing)();
    int missing = ((FnMissing)GetProcAddress(proxy, "OrcsteamMissingCount"))();
    printf("exports without implementation: %d\n", missing);
    if (missing) fails++;

    // XOnlineStartup(), XNetStartup(NULL): plain successes with balanced stack.
    const struct { unsigned ord; int nargs; } calls[] = {{5310, 0}, {51, 1}};
    for (auto c : calls) {
        bool e;
        DWORD a = CallChecked(Ord(proxy, c.ord), c.nargs, e);
        printf("#%u = %08lX esp %s\n", c.ord, a, e ? "ok" : "BAD");
        if (a != 0 || !e) fails++;
    }
    fails += TestCompat(proxy);

    // Overridden: XUserGetSigninState(0) must say "signed in to Live".
    bool esp;
    DWORD state = CallChecked(GetProcAddress(proxy, MAKEINTRESOURCEA(5262)), 1, esp);
    printf("#5262 signin state=%lu esp %s\n", state, esp ? "ok" : "BAD");
    if (state != 2 || !esp) fails++;

    // XUserGetSigninInfo(0, 0, &info): online xuid, Live state, some name.
    struct { unsigned long long xuid; DWORD flags, state, guest, sponsor; char name[16]; } info = {};
    DWORD r = ((FnSigninInfo)GetProcAddress(proxy, MAKEINTRESOURCEA(5267)))(0, 0, &info);
    printf("#5267 r=%lu xuid=%016llX state=%lu name='%s'\n", r, info.xuid, info.state, info.name);
    if (r != 0 || info.state != 2 || (info.xuid >> 48) != 0x0009 || !info.name[0]) fails++;

    // Sessions: create as host with an overlapped, start, query result, search, close.
    struct { ULONG_PTR low, high, ctx; HANDLE ev; void* routine; DWORD_PTR cctx; DWORD ext; } ov = {};
    ov.ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    BYTE sinfo[60] = {};
    unsigned long long nonce = 0;
    HANDLE session = nullptr;
    typedef DWORD(WINAPI * FnCreate)(DWORD, DWORD, DWORD, DWORD, void*, void*, void*, HANDLE*);
    DWORD before, after;
    __asm mov before, esp
    r = ((FnCreate)GetProcAddress(proxy, MAKEINTRESOURCEA(5300)))(0x2D, 0, 4, 0, &nonce, sinfo, &ov, &session);
    __asm mov after, esp
    bool signaled = WaitForSingleObject(ov.ev, 0) == WAIT_OBJECT_0;
    printf("#5300 r=%lu ov.low=%lu signaled=%d handle=%p kid0=%02X nonce=%016llX esp %s\n", r, (DWORD)ov.low,
           signaled, session, sinfo[0], nonce, before == after ? "ok" : "BAD");
    if (r != ERROR_IO_PENDING || ov.low != 0 || !signaled || !session || (sinfo[0] & 0xF0) != 0x80 || !nonce ||
        before != after)
        fails++;

    typedef DWORD(WINAPI * FnOvResult)(void*, DWORD*, BOOL);
    DWORD res = 99;
    r = ((FnOvResult)GetProcAddress(proxy, MAKEINTRESOURCEA(1083)))(&ov, &res, FALSE);
    printf("#1083 r=%lu res=%lu\n", r, res);
    if (r != 0) fails++;

    typedef DWORD(WINAPI * FnStart)(HANDLE, DWORD, void*);
    r = ((FnStart)GetProcAddress(proxy, MAKEINTRESOURCEA(5318)))(session, 0, nullptr);
    printf("#5318 start r=%lu\n", r);
    if (r != 0) fails++;

    typedef DWORD(WINAPI * FnSearch)(DWORD, DWORD, DWORD, WORD, WORD, void*, void*, DWORD*, void*, void*);
    DWORD cb = 0;
    __asm mov before, esp
    r = ((FnSearch)GetProcAddress(proxy, MAKEINTRESOURCEA(5321)))(0, 0, 10, 0, 0, nullptr, nullptr, &cb, nullptr, nullptr);
    __asm mov after, esp
    printf("#5321 search r=%lu cb=%lu esp %s\n", r, cb, before == after ? "ok" : "BAD");
    if (r != ERROR_INSUFFICIENT_BUFFER || cb <= 8 || before != after) fails++;

    // Host migration: with XUSER_INDEX_NONE (0xFE) the info is the new host's and must stay untouched;
    // with our user index we become the host and the info gets our address.
    typedef DWORD(WINAPI * FnMigrate)(HANDLE, DWORD, void*, void*);
    BYTE other[60];
    memcpy(other, sinfo, sizeof(other));
    memset(other + 8, 0xAB, 36);  // XNADDR of someone else
    BYTE given[60];
    memcpy(given, other, sizeof(given));
    __asm mov before, esp
    r = ((FnMigrate)GetProcAddress(proxy, MAKEINTRESOURCEA(5323)))(session, 0xFE, given, nullptr);
    __asm mov after, esp
    bool kept = memcmp(given, other, sizeof(given)) == 0;
    DWORD r2 = ((FnMigrate)GetProcAddress(proxy, MAKEINTRESOURCEA(5323)))(session, 0, given, nullptr);
    bool ours = memcmp(given + 8, sinfo + 8, 36) == 0;
    printf("#5323 migrate: other host r=%lu kept=%d, we host r=%lu ours=%d, esp %s\n", r, kept, r2, ours,
           before == after ? "ok" : "BAD");
    if (r != 0 || !kept || r2 != 0 || !ours || before != after) fails++;

    typedef BOOL(WINAPI * FnClose)(HANDLE);
    BOOL closed = ((FnClose)GetProcAddress(proxy, MAKEINTRESOURCEA(5251)))(session);
    r = ((FnStart)GetProcAddress(proxy, MAKEINTRESOURCEA(5318)))(session, 0, nullptr);
    printf("#5251 close=%d, start after close r=%lu\n", closed, r);
    if (!closed || r != ERROR_INVALID_HANDLE) fails++;

    SetLastError(1234);
    ((Fn0)GetProcAddress(proxy, MAKEINTRESOURCEA(5310)))();
    printf("GetLastError preserved through proxy: %lu\n", GetLastError());
    if (GetLastError() != 1234) fails++;

    // Functions the stub implemented with the wrong stack cleanup (tools/audit_stub.ps1): ours must balance.
    {
        typedef INT(WINAPI * FnToString)(IN_ADDR, char*, INT);
        typedef DWORD(WINAPI * FnInvite)(DWORD, DWORD, const void*, const wchar_t*, void*);
        IN_ADDR ina;
        ina.s_addr = 0x0302010A;  // 10.1.2.3
        char text[32] = {};
        DWORD b1, a1, b2, a2;
        __asm mov b1, esp
        INT r1 = ((FnToString)Ord(proxy, 62))(ina, text, sizeof(text));
        __asm mov a1, esp
        unsigned long long xuid = 1;
        __asm mov b2, esp
        DWORD r2 = ((FnInvite)Ord(proxy, 5316))(0, 1, &xuid, L"hi", nullptr);
        __asm mov a2, esp
        printf("#62 r=%d '%s' esp %s; #5316 r=%lu esp %s\n", r1, text, b1 == a1 ? "ok" : "BAD", r2, b2 == a2 ? "ok" : "BAD");
        if (r1 != 0 || strcmp(text, "10.1.2.3") || b1 != a1 || r2 != 0 || b2 != a2) fails++;
    }

    fails += TestTransport(proxy);
    if (argc > 1 && !strcmp(argv[1], "steam")) fails += TestSteam(proxy);

    if (steamShutdown) steamShutdown();
    printf(fails ? "FAIL\n" : "PASS\n");
    return fails;
}
