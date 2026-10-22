// XSession* implementation backed by Steam lobbies.
// Host: every matchmaking session gets a public Steam lobby carrying the XSESSION_INFO and the
// game's search contexts. Client: XSessionSearch lists those lobbies; creating a client session
// for a found host joins its lobby. Party sessions (no matchmaking flag) stay local.
// Async calls complete immediately unless they wait on Steam (search), which completes from the pump.

#define WIN32_LEAN_AND_MEAN
#include "xlive_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "overrides.h"
#include "steam.h"

static const int kMaxSessions = 16;
static const int kMaxMembers = 16;
static const int kMaxContexts = 8;
static const int kMaxProperties = 16;
static const int kMaxFound = 64;
static const DWORD kSyncSearchTimeoutMs = 8000;
static const char kLobbyTag[] = "orcsteam";
static const char kLobbyVersion[] = "1";

static const DWORD kCreateHost = 0x1;
static const DWORD kCreateUsesMatchmaking = 0x8;
static const DWORD kContextGameType = 0x800A, kContextGameMode = 0x800B;
// Real GFWL added the host's gamertag to every search result; ORC reads it without a null check.
static const DWORD kPropertyHostName = 0x40008109;
static const int kHostNameChars = 32;  // ORC copies at most 0x20 chars

struct XUSER_CONTEXT { DWORD dwContextId; DWORD dwValue; };
struct XUSER_DATA {
    BYTE type;
    union {
        LONG nData;
        LONGLONG i64Data;
        double dblData;
        struct { DWORD cbData; LPWSTR pwszData; } string;
        float fData;
        struct { DWORD cbData; PBYTE pbData; } binary;
        FILETIME ftData;
    };
};
struct XUSER_PROPERTY { DWORD dwPropertyId; XUSER_DATA value; };
struct XSESSION_SEARCHRESULT {
    XSESSION_INFO info;
    DWORD dwOpenPublicSlots, dwOpenPrivateSlots, dwFilledPublicSlots, dwFilledPrivateSlots;
    DWORD cProperties, cContexts;
    XUSER_PROPERTY* pProperties;
    XUSER_CONTEXT* pContexts;
};
static const BYTE kDataInt32 = 1, kDataInt64 = 2, kDataUnicode = 4;

struct Attributes {
    int contextCount, propertyCount;
    XUSER_CONTEXT contexts[kMaxContexts];
    XUSER_PROPERTY properties[kMaxProperties];  // numeric only (INT32/INT64), which is all ORC sets
};

struct Session {
    HANDLE handle;  // a real event handle, so it is unique and closable
    DWORD generation;
    DWORD flags;
    DWORD publicSlots, privateSlots;
    XSESSION_INFO info;
    ULONGLONG nonce;
    bool host;
    bool started;
    int memberCount;
    XUID members[kMaxMembers];
    Attributes attrs;
    uint64 lobby;  // Steam lobby id, 0 if none (yet)
};

struct FoundLobby { XNKID kid; uint64 lobby; };

struct Search {
    bool active;
    DWORD wantResults;
    DWORD wantType, wantMode;  // 0xFFFFFFFF = any
    DWORD* cbResults;
    XSESSION_SEARCHRESULT_HEADER* results;
    XOVERLAPPED* ov;
    HANDLE syncDone;
    DWORD syncResult;
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static Session g_sessions[kMaxSessions];
static DWORD g_generation;
static Attributes g_userAttrs;  // what the game set with XUserSetContext/XUserSetProperty
static FoundLobby g_found[kMaxFound];
static Search g_search;

static void Lock() { AcquireSRWLockExclusive(&g_lock); }
static void Unlock() { ReleaseSRWLockExclusive(&g_lock); }

static Session* Find(HANDLE h) {
    if (!h) return nullptr;
    for (Session& s : g_sessions)
        if (s.handle == h) return &s;
    return nullptr;
}

// lobby=friends in orcsteam.ini: hosted games are visible to Steam friends and invitees only
// (they also no longer show up in anyone's in-game search). Default: public.
static int HostedLobbyType() {
    char value[16];
    OrcsteamIniString("lobby", "public", value, sizeof(value));
    return _stricmp(value, "friends") == 0 ? kLobbyFriendsOnly : kLobbyPublic;
}

// Steam rich presence from the session state. Without the publisher's localization tokens Steam shows
// "status" in the friend's "View game info" dialog; the player group keeps lobby mates together in
// the friends list. Must be called without g_lock held.
static void UpdatePresence() {
    static char lastStatus[64], lastGroup[24], lastSize[8];
    const SteamApi* steam = Steam();
    if (!steam) return;
    bool party = false, match = false, started = false;
    uint64 group = 0;
    int players = 0;
    AcquireSRWLockShared(&g_lock);
    for (Session& s : g_sessions) {
        if (!s.handle) continue;
        if (!(s.flags & kCreateUsesMatchmaking)) {
            party = true;
            continue;
        }
        match = true;
        started |= s.started;
        if (s.lobby) group = s.lobby;
        if (s.memberCount > players) players = s.memberCount;
    }
    ReleaseSRWLockShared(&g_lock);

    char status[64] = "", groupText[24] = "", size[8] = "";
    if (started && players > 1) sprintf_s(status, "On a mission (%d players)", players);
    else if (started) strcpy_s(status, "On a mission");
    else if (match) strcpy_s(status, "In an online lobby");
    else if (party) strcpy_s(status, "In the online menu");
    if (group) {  // we are in a Steam lobby: group with its other members (we count at least ourselves)
        sprintf_s(groupText, "%016llX", group);
        sprintf_s(size, "%d", players > 1 ? players : 1);
    }
    if (strcmp(status, lastStatus)) {
        steam->friends->SetRichPresence("status", status);  // empty value removes the key
        strcpy_s(lastStatus, status);
        OverrideLog("presence: '%s'", status);
    }
    if (strcmp(groupText, lastGroup) || strcmp(size, lastSize)) {
        steam->friends->SetRichPresence("steam_player_group", groupText);
        steam->friends->SetRichPresence("steam_player_group_size", size);
        strcpy_s(lastGroup, groupText);
        strcpy_s(lastSize, size);
    }
}

static void Random(void* out, size_t n) {
    typedef BOOLEAN(WINAPI * GenRandom)(PVOID, ULONG);  // RtlGenRandom
    static GenRandom gen = (GenRandom)GetProcAddress(LoadLibraryA("advapi32.dll"), "SystemFunction036");
    if (!gen || !gen(out, (ULONG)n)) {
        BYTE* p = (BYTE*)out;
        for (size_t i = 0; i < n; i++) p[i] = (BYTE)(GetTickCount() * 31 + i * 17);
    }
}

static void Hex(const void* data, size_t n, char* out) {
    const BYTE* p = (const BYTE*)data;
    for (size_t i = 0; i < n; i++) sprintf_s(out + i * 2, 3, "%02X", p[i]);
}

static bool Unhex(const char* in, void* data, size_t n) {
    if (!in || strlen(in) != n * 2) return false;
    BYTE* p = (BYTE*)data;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf_s(in + i * 2, "%2X", &v) != 1) return false;
        p[i] = (BYTE)v;
    }
    return true;
}

// Finish an async call right away. Returns what the X* function itself must return.
static DWORD Complete(XOVERLAPPED* ov, DWORD result, DWORD value = 0) {
    if (!ov) return result;
    ov->InternalLow = result;
    ov->InternalHigh = value;
    ov->dwExtendedError = result == ERROR_SUCCESS ? 0 : HRESULT_FROM_WIN32(result);
    if (ov->hEvent) SetEvent(ov->hEvent);
    if (ov->pCompletionRoutine) ov->pCompletionRoutine(result, value, ov);
    return ERROR_IO_PENDING;
}

static void AddMember(Session& s, XUID xuid) {
    for (int i = 0; i < s.memberCount; i++)
        if (s.members[i] == xuid) return;
    if (s.memberCount < kMaxMembers) s.members[s.memberCount++] = xuid;
}

static void RemoveMember(Session& s, XUID xuid) {
    for (int i = 0; i < s.memberCount; i++)
        if (s.members[i] == xuid) {
            s.members[i] = s.members[--s.memberCount];
            return;
        }
}

// ---- lobby data ----

static void FormatAttrs(const Attributes& a, char* ctx, size_t ctxCap, char* props, size_t propsCap) {
    ctx[0] = props[0] = 0;
    for (int i = 0; i < a.contextCount; i++) {
        char item[32];
        sprintf_s(item, "%lX=%lu;", a.contexts[i].dwContextId, a.contexts[i].dwValue);
        strcat_s(ctx, ctxCap, item);
    }
    for (int i = 0; i < a.propertyCount; i++) {
        char item[48];
        const XUSER_PROPERTY& p = a.properties[i];
        if (p.value.type == kDataInt64) sprintf_s(item, "%lX=%lld;", p.dwPropertyId, p.value.i64Data);
        else sprintf_s(item, "%lX=%ld;", p.dwPropertyId, p.value.nData);
        strcat_s(props, propsCap, item);
    }
}

static void ParseAttrs(const char* ctx, const char* props, Attributes& a) {
    memset(&a, 0, sizeof(a));
    for (const char* p = ctx; p && *p && a.contextCount < kMaxContexts;) {
        unsigned long id, value;
        if (sscanf_s(p, "%lX=%lu", &id, &value) != 2) break;
        a.contexts[a.contextCount++] = {id, value};
        p = strchr(p, ';');
        if (p) p++;
    }
    for (const char* p = props; p && *p && a.propertyCount < kMaxProperties;) {
        unsigned long id;
        long long value;
        if (sscanf_s(p, "%lX=%lld", &id, &value) != 2) break;
        XUSER_PROPERTY& prop = a.properties[a.propertyCount++];
        memset(&prop, 0, sizeof(prop));
        prop.dwPropertyId = id;
        prop.value.type = (BYTE)(id >> 28);
        if (prop.value.type == kDataInt64) prop.value.i64Data = value;
        else prop.value.nData = (LONG)value;
        p = strchr(p, ';');
        if (p) p++;
    }
}

static DWORD ContextValue(const Attributes& a, DWORD id) {
    for (int i = 0; i < a.contextCount; i++)
        if (a.contexts[i].dwContextId == id) return a.contexts[i].dwValue;
    return 0xFFFFFFFF;
}

// Caller holds g_lock. Pushes the session's current state into its lobby.
static void PublishLobby(const Session& s) {
    const SteamApi* steam = Steam();
    if (!steam || !s.lobby) return;
    CSteamID lobby(s.lobby);
    char info[sizeof(XSESSION_INFO) * 2 + 1], ctx[256], props[640], num[16];
    Hex(&s.info, sizeof(s.info), info);
    FormatAttrs(s.attrs, ctx, sizeof(ctx), props, sizeof(props));
    steam->mm->SetLobbyData(lobby, kLobbyTag, kLobbyVersion);
    steam->mm->SetLobbyData(lobby, "info", info);
    steam->mm->SetLobbyData(lobby, "ctx", ctx);
    steam->mm->SetLobbyData(lobby, "props", props);
    sprintf_s(num, "%lu", s.publicSlots);
    steam->mm->SetLobbyData(lobby, "pub", num);
    sprintf_s(num, "%lu", s.privateSlots);
    steam->mm->SetLobbyData(lobby, "priv", num);
    sprintf_s(num, "%d", s.memberCount);
    steam->mm->SetLobbyData(lobby, "filled", num);
    sprintf_s(num, "%lX", ContextValue(s.attrs, kContextGameType));
    steam->mm->SetLobbyData(lobby, "type", num);
    sprintf_s(num, "%lX", ContextValue(s.attrs, kContextGameMode));
    steam->mm->SetLobbyData(lobby, "mode", num);
    steam->mm->SetLobbyData(lobby, "started", s.started ? "1" : "0");
    steam->mm->SetLobbyData(lobby, "name", LocalGamertag());  // ASCII, shown by the client's game as-is
}

static void* SessionCtx(const Session& s) {
    return (void*)(ULONG_PTR)(((s.generation & 0xFFFFFF) << 8) | (DWORD)(&s - g_sessions));
}

static Session* FromCtx(void* ctx) {  // caller holds g_lock
    DWORD v = (DWORD)(ULONG_PTR)ctx;
    Session& s = g_sessions[v & 0xFF];
    return s.handle && (s.generation & 0xFFFFFF) == (v >> 8) ? &s : nullptr;
}

static void OnLobbyCreated(void* ctx, const void* result, bool failed) {
    const LobbyCreated_t* r = (const LobbyCreated_t*)result;
    const SteamApi* steam = Steam();
    Lock();
    Session* s = FromCtx(ctx);
    if (failed || !r || r->m_eResult != 1) {
        OverrideLog("!! lobby create failed (io %d, result %d)", failed, r ? r->m_eResult : -1);
    } else if (!s) {
        if (steam) steam->mm->LeaveLobby(CSteamID(r->m_ulSteamIDLobby));  // session went away meanwhile
    } else {
        s->lobby = r->m_ulSteamIDLobby;
        PublishLobby(*s);
        OverrideLog("lobby %016llX created for session %p", s->lobby, s->handle);
    }
    Unlock();
    UpdatePresence();
}

static void OnLobbyEntered(void* ctx, const void* result, bool failed) {
    const LobbyEnter_t* r = (const LobbyEnter_t*)result;
    const SteamApi* steam = Steam();
    Lock();
    Session* s = FromCtx(ctx);
    bool ok = !failed && r && r->m_EChatRoomEnterResponse == 1;
    if (!ok) {
        OverrideLog("!! lobby join failed (io %d, response %lu)", failed, r ? r->m_EChatRoomEnterResponse : 0);
    } else if (!s) {
        if (steam) steam->mm->LeaveLobby(CSteamID(r->m_ulSteamIDLobby));
    } else {
        s->lobby = r->m_ulSteamIDLobby;
        OverrideLog("joined lobby %016llX for session %p", s->lobby, s->handle);
    }
    Unlock();
    UpdatePresence();
}

static void LeaveLobbyOf(Session& s) {  // caller holds g_lock
    if (!s.lobby) return;
    if (const SteamApi* steam = Steam()) steam->mm->LeaveLobby(CSteamID(s.lobby));
    OverrideLog("left lobby %016llX", s.lobby);
    s.lobby = 0;
}

void SessionsPump(const SteamApi& steam) {
    // Accept P2P from everyone in our lobbies (the P2P request callback covers the rest).
    DWORD accounts[64];
    int n = 0;
    Lock();
    for (Session& s : g_sessions) {
        if (!s.handle || !s.lobby) continue;
        CSteamID lobby(s.lobby);
        int members = steam.mm->GetNumLobbyMembers(lobby);
        for (int i = 0; i < members && n < 64; i++) accounts[n++] = steam.mm->GetLobbyMemberByIndex(lobby, i).AccountId();
    }
    Unlock();
    for (int i = 0; i < n; i++) NetAcceptPeer(accounts[i]);
}

void ActiveSessionKid(XNKID* kid) {
    memset(kid, 0, sizeof(*kid));
    Lock();
    for (Session& s : g_sessions)
        if (s.handle && (s.flags & kCreateUsesMatchmaking)) *kid = s.info.sessionID;
    Unlock();
}

// ---- user contexts / properties (search parameters) ----

static VOID WINAPI My_XUserSetContext(DWORD userIndex, DWORD contextId, DWORD value) {
    if (userIndex != 0) return;
    Lock();
    Attributes& a = g_userAttrs;
    int i = 0;
    while (i < a.contextCount && a.contexts[i].dwContextId != contextId) i++;
    if (i == a.contextCount && a.contextCount < kMaxContexts) a.contextCount++;
    if (i < a.contextCount) a.contexts[i] = {contextId, value};
    Unlock();
}

static VOID WINAPI My_XUserSetProperty(DWORD userIndex, DWORD propertyId, DWORD cbValue, const void* value) {
    if (userIndex != 0 || !value) return;
    BYTE type = (BYTE)(propertyId >> 28);
    if (type != kDataInt32 && type != kDataInt64) return;
    Lock();
    Attributes& a = g_userAttrs;
    int i = 0;
    while (i < a.propertyCount && a.properties[i].dwPropertyId != propertyId) i++;
    if (i == a.propertyCount && a.propertyCount < kMaxProperties) a.propertyCount++;
    if (i < a.propertyCount) {
        XUSER_PROPERTY& p = a.properties[i];
        memset(&p, 0, sizeof(p));
        p.dwPropertyId = propertyId;
        p.value.type = type;
        if (type == kDataInt64 && cbValue >= 8) p.value.i64Data = *(const LONGLONG*)value;
        else p.value.nData = *(const LONG*)value;
    }
    Unlock();
}

// ---- sessions ----

static DWORD WINAPI My_XSessionCreate(DWORD flags, DWORD userIndex, DWORD maxPublic, DWORD maxPrivate,
                                      ULONGLONG* nonce, XSESSION_INFO* info, XOVERLAPPED* ov, HANDLE* ph) {
    if (!ph || !info || !nonce) return ERROR_INVALID_PARAMETER;
    const SteamApi* steam = Steam();
    Lock();
    Session* s = nullptr;
    for (Session& c : g_sessions)
        if (!c.handle) { s = &c; break; }
    if (!s) { Unlock(); return ERROR_NOT_ENOUGH_MEMORY; }

    memset(s, 0, sizeof(*s));
    s->handle = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    s->generation = ++g_generation;
    s->flags = flags;
    s->publicSlots = maxPublic;
    s->privateSlots = maxPrivate;
    s->host = (flags & kCreateHost) != 0;
    s->attrs = g_userAttrs;
    if (s->host) {
        // Host: we invent the session identity. A joining client gets it in *info from the search result.
        Random(&s->info.sessionID, sizeof(s->info.sessionID));
        s->info.sessionID.ab[0] = (s->info.sessionID.ab[0] & 0x0F) | kXnkidOnlinePeer;
        Random(&s->info.keyExchangeKey, sizeof(s->info.keyExchangeKey));
        FillLocalXnAddr(&s->info.hostAddress);
        Random(&s->nonce, sizeof(s->nonce));
        *info = s->info;
        *nonce = s->nonce;
    } else {
        s->info = *info;
        s->nonce = *nonce;
    }
    *ph = s->handle;

    bool matchmaking = (flags & kCreateUsesMatchmaking) != 0;
    int lobbyType = HostedLobbyType();
    if (matchmaking && steam && s->host) {
        SteamAsync(steam->mm->CreateLobby(lobbyType, (int)(maxPublic + maxPrivate)), LobbyCreated_t::k_iCallback,
                   sizeof(LobbyCreated_t), OnLobbyCreated, SessionCtx(*s));
    } else if (matchmaking && steam) {
        uint64 lobby = 0;
        for (FoundLobby& f : g_found)
            if (f.lobby && !memcmp(&f.kid, &s->info.sessionID, sizeof(XNKID))) lobby = f.lobby;
        NetAcceptPeer(XnAddrAccountId(&s->info.hostAddress));
        if (lobby)
            SteamAsync(steam->mm->JoinLobby(CSteamID(lobby)), LobbyEnter_t::k_iCallback, sizeof(LobbyEnter_t),
                       OnLobbyEntered, SessionCtx(*s));
        else
            OverrideLog("!! client session for a host we did not find in a search");
    }
    OverrideLog("session %p created: flags %03lX, slots %lu+%lu, %s%s, type %lX mode %lX", s->handle, flags, maxPublic,
                maxPrivate, s->host ? "host" : "client",
                matchmaking ? (s->host && lobbyType == kLobbyFriendsOnly ? ", steam lobby (friends only)" : ", steam lobby") : "",
                ContextValue(s->attrs, kContextGameType), ContextValue(s->attrs, kContextGameMode));
    Unlock();
    UpdatePresence();
    return Complete(ov, ERROR_SUCCESS);
}

static DWORD WINAPI My_XSessionDelete(HANDLE h, XOVERLAPPED* ov) {
    Lock();
    Session* s = Find(h);
    if (s) {
        LeaveLobbyOf(*s);
        s->memberCount = 0;
        s->started = false;
    }
    Unlock();
    UpdatePresence();
    return Complete(ov, s ? ERROR_SUCCESS : ERROR_INVALID_HANDLE);
}

static DWORD WINAPI My_XSessionStart(HANDLE h, DWORD flags, XOVERLAPPED* ov) {
    Lock();
    Session* s = Find(h);
    if (s) {
        s->started = true;
        if (s->host) PublishLobby(*s);
    }
    Unlock();
    UpdatePresence();
    return Complete(ov, s ? ERROR_SUCCESS : ERROR_INVALID_HANDLE);
}

static DWORD WINAPI My_XSessionEnd(HANDLE h, XOVERLAPPED* ov) {
    Lock();
    Session* s = Find(h);
    if (s) {
        s->started = false;
        if (s->host) PublishLobby(*s);
    }
    Unlock();
    UpdatePresence();
    return Complete(ov, s ? ERROR_SUCCESS : ERROR_INVALID_HANDLE);
}

static DWORD WINAPI My_XSessionModify(HANDLE h, DWORD flags, DWORD maxPublic, DWORD maxPrivate, XOVERLAPPED* ov) {
    Lock();
    Session* s = Find(h);
    if (s) {
        s->flags = (s->flags & (kCreateHost | kCreateUsesMatchmaking)) | (flags & ~(kCreateHost | kCreateUsesMatchmaking));
        s->publicSlots = maxPublic;
        s->privateSlots = maxPrivate;
        if (s->host) PublishLobby(*s);
    }
    Unlock();
    UpdatePresence();
    return Complete(ov, s ? ERROR_SUCCESS : ERROR_INVALID_HANDLE);
}

static DWORD WINAPI My_XSessionJoinLocal(HANDLE h, DWORD count, const DWORD* userIndexes, const BOOL* privateSlots,
                                        XOVERLAPPED* ov) {
    Lock();
    Session* s = Find(h);
    if (s) {
        for (DWORD i = 0; i < count; i++)
            if (userIndexes && userIndexes[i] == 0) AddMember(*s, LocalXuid());
        if (s->host) PublishLobby(*s);
    }
    Unlock();
    UpdatePresence();
    return Complete(ov, s ? ERROR_SUCCESS : ERROR_INVALID_HANDLE);
}

static DWORD WINAPI My_XSessionLeaveLocal(HANDLE h, DWORD count, const DWORD* userIndexes, XOVERLAPPED* ov) {
    Lock();
    Session* s = Find(h);
    if (s) {
        for (DWORD i = 0; i < count; i++)
            if (userIndexes && userIndexes[i] == 0) RemoveMember(*s, LocalXuid());
        if (s->host) PublishLobby(*s);
    }
    Unlock();
    UpdatePresence();
    return Complete(ov, s ? ERROR_SUCCESS : ERROR_INVALID_HANDLE);
}

static DWORD WINAPI My_XSessionJoinRemote(HANDLE h, DWORD count, const XUID* xuids, const BOOL* privateSlots,
                                         XOVERLAPPED* ov) {
    Lock();
    Session* s = Find(h);
    if (s && xuids) {
        for (DWORD i = 0; i < count; i++) {
            AddMember(*s, xuids[i]);
            OverrideLog("session %p: remote player %016llX joined", h, xuids[i]);
        }
        if (s->host) PublishLobby(*s);
    }
    Unlock();
    UpdatePresence();
    return Complete(ov, s ? ERROR_SUCCESS : ERROR_INVALID_HANDLE);
}

static DWORD WINAPI My_XSessionLeaveRemote(HANDLE h, DWORD count, const XUID* xuids, XOVERLAPPED* ov) {
    Lock();
    Session* s = Find(h);
    if (s && xuids) {
        for (DWORD i = 0; i < count; i++) {
            RemoveMember(*s, xuids[i]);
            OverrideLog("session %p: remote player %016llX left", h, xuids[i]);
        }
        if (s->host) PublishLobby(*s);
    }
    Unlock();
    UpdatePresence();
    return Complete(ov, s ? ERROR_SUCCESS : ERROR_INVALID_HANDLE);
}

static DWORD WINAPI My_XSessionMigrateHost(HANDLE h, DWORD userIndex, XSESSION_INFO* info, XOVERLAPPED* ov) {
    Lock();
    Session* s = Find(h);
    if (s && info) {
        s->host = true;
        FillLocalXnAddr(&s->info.hostAddress);
        *info = s->info;
        OverrideLog("session %p: we are the host now", h);
        PublishLobby(*s);
    }
    Unlock();
    UpdatePresence();
    return Complete(ov, s ? ERROR_SUCCESS : ERROR_INVALID_HANDLE);
}

// ---- helpers for invites.cpp ----

DWORD CompleteOverlapped(XOVERLAPPED* ov, DWORD result, DWORD value) {
    return Complete(ov, result, value);
}

uint64 HostedLobby() {
    uint64 lobby = 0;
    Lock();
    for (Session& s : g_sessions)
        if (s.handle && s.host && s.lobby) lobby = s.lobby;
    Unlock();
    return lobby;
}

bool LobbyInUse(uint64 lobby) {
    bool used = false;
    Lock();
    for (Session& s : g_sessions)
        if (s.handle && s.lobby == lobby) used = true;
    Unlock();
    return used;
}

// A lobby we learned about from an invite: the client session for its kid will join it.
void RegisterFoundLobby(const XNKID& kid, uint64 lobby) {
    Lock();
    FoundLobby* slot = &g_found[0];
    for (FoundLobby& f : g_found) {
        if (!memcmp(&f.kid, &kid, sizeof(kid))) { slot = &f; break; }
        if (!f.lobby) { slot = &f; break; }
    }
    *slot = {kid, lobby};
    Unlock();
}

static DWORD WINAPI My_XSessionWriteStats(HANDLE h, XUID xuid, DWORD numViews, const void* views, XOVERLAPPED* ov) {
    return Complete(ov, ERROR_SUCCESS);  // no leaderboards
}

// ---- search ----

// Per result: the result itself, its contexts, its properties (+1 for the host name) and the name text.
static const DWORD kResultSlotSize = sizeof(XSESSION_SEARCHRESULT) + kMaxContexts * sizeof(XUSER_CONTEXT) +
                                     (kMaxProperties + 1) * sizeof(XUSER_PROPERTY) + kHostNameChars * sizeof(WCHAR);

// Pump thread: lobby list arrived. Fill the game's buffer and complete its overlapped.
static void OnLobbyList(void* ctx, const void* result, bool failed) {
    const SteamApi* steam = Steam();
    const LobbyMatchList_t* r = (const LobbyMatchList_t*)result;
    Lock();
    Search sr = g_search;
    g_search.active = false;
    if (!sr.active) { Unlock(); return; }

    DWORD found = (!failed && r && steam) ? r->m_nLobbiesMatching : 0;
    DWORD cap = *sr.cbResults;
    DWORD overhead = sizeof(XSESSION_SEARCHRESULT_HEADER) + 8;  // header + alignment slack for the property area
    DWORD maxFit = cap > overhead ? (cap - overhead) / kResultSlotSize : 0;
    XSESSION_SEARCHRESULT* out = (XSESSION_SEARCHRESULT*)((BYTE*)sr.results + sizeof(XSESSION_SEARCHRESULT_HEADER));
    BYTE* extra = (BYTE*)(out + (maxFit ? maxFit : 0));
    extra = (BYTE*)(((ULONG_PTR)extra + 7) & ~(ULONG_PTR)7);
    DWORD count = 0, skipped = 0;
    memset(g_found, 0, sizeof(g_found));

    for (DWORD i = 0; i < found && count < maxFit && count < sr.wantResults; i++) {
        CSteamID lobby = steam->mm->GetLobbyByIndex((int)i);
        XSESSION_INFO info;
        if (!Unhex(steam->mm->GetLobbyData(lobby, "info"), &info, sizeof(info))) { skipped++; continue; }
        // Our own game is skipped, except for self-tests (ORCSTEAM_SEARCH_SELF=1).
        if (XnAddrAccountId(&info.hostAddress) == LocalAccountId() && !GetEnvironmentVariableA("ORCSTEAM_SEARCH_SELF", nullptr, 0)) {
            skipped++;
            continue;
        }
        Attributes attrs;
        ParseAttrs(steam->mm->GetLobbyData(lobby, "ctx"), steam->mm->GetLobbyData(lobby, "props"), attrs);
        if ((sr.wantType != 0xFFFFFFFF && ContextValue(attrs, kContextGameType) != sr.wantType) ||
            (sr.wantMode != 0xFFFFFFFF && ContextValue(attrs, kContextGameMode) != sr.wantMode)) {
            skipped++;
            continue;
        }
        DWORD pub = (DWORD)atoi(steam->mm->GetLobbyData(lobby, "pub"));
        DWORD priv = (DWORD)atoi(steam->mm->GetLobbyData(lobby, "priv"));
        DWORD filled = (DWORD)atoi(steam->mm->GetLobbyData(lobby, "filled"));
        if (filled > pub) filled = pub;

        XSESSION_SEARCHRESULT& res = out[count];
        memset(&res, 0, sizeof(res));
        res.info = info;
        res.dwFilledPublicSlots = filled;
        res.dwOpenPublicSlots = pub - filled;
        res.dwOpenPrivateSlots = priv;
        res.cContexts = attrs.contextCount;
        res.pContexts = (XUSER_CONTEXT*)extra;
        memcpy(extra, attrs.contexts, attrs.contextCount * sizeof(XUSER_CONTEXT));
        extra += kMaxContexts * sizeof(XUSER_CONTEXT);
        XUSER_PROPERTY* props = (XUSER_PROPERTY*)extra;
        memcpy(props, attrs.properties, attrs.propertyCount * sizeof(XUSER_PROPERTY));
        extra += (kMaxProperties + 1) * sizeof(XUSER_PROPERTY);
        WCHAR* name = (WCHAR*)extra;
        extra += kHostNameChars * sizeof(WCHAR);
        const char* utf8 = steam->mm->GetLobbyData(lobby, "name");
        if (!utf8 || !*utf8 || !MultiByteToWideChar(CP_UTF8, 0, utf8, -1, name, kHostNameChars)) wcscpy_s(name, kHostNameChars, L"Player");
        name[kHostNameChars - 1] = 0;
        XUSER_PROPERTY& hostName = props[attrs.propertyCount];
        memset(&hostName, 0, sizeof(hostName));
        hostName.dwPropertyId = kPropertyHostName;
        hostName.value.type = kDataUnicode;
        hostName.value.string.cbData = (DWORD)((wcslen(name) + 1) * sizeof(WCHAR));
        hostName.value.string.pwszData = name;
        res.cProperties = attrs.propertyCount + 1;
        res.pProperties = props;

        if (count < (DWORD)kMaxFound) g_found[count] = {info.sessionID, lobby.v};
        OverrideLog("  found lobby %016llX: host account %lu, %lu/%lu players, type %lX mode %lX", lobby.v,
                    XnAddrAccountId(&info.hostAddress), filled, pub, ContextValue(attrs, kContextGameType),
                    ContextValue(attrs, kContextGameMode));
        count++;
    }
    sr.results->dwSearchResults = count;
    sr.results->pResults = out;
    OverrideLog("session search done: %lu lobbies from steam, %lu returned, %lu skipped%s", found, count, skipped,
                failed ? " (steam call failed)" : "");
    Unlock();

    if (sr.ov) Complete(sr.ov, ERROR_SUCCESS);
    if (sr.syncDone) {
        g_search.syncResult = ERROR_SUCCESS;
        SetEvent(sr.syncDone);
    }
}

static DWORD WINAPI My_XSessionSearch(DWORD procedureIndex, DWORD userIndex, DWORD numResults, WORD numProperties,
                                      WORD numContexts, XUSER_PROPERTY* properties, XUSER_CONTEXT* contexts,
                                      DWORD* cbResults, XSESSION_SEARCHRESULT_HEADER* results, XOVERLAPPED* ov) {
    if (!cbResults || !numResults) return ERROR_INVALID_PARAMETER;
    DWORD need = sizeof(XSESSION_SEARCHRESULT_HEADER) + numResults * kResultSlotSize + 8;
    if (*cbResults < need || !results) {
        *cbResults = need;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    const SteamApi* steam = Steam();
    results->dwSearchResults = 0;
    results->pResults = nullptr;
    if (!steam) return Complete(ov, ERROR_SUCCESS);

    Lock();
    if (g_search.active) {  // one search at a time: answer the newer one with nothing
        Unlock();
        return Complete(ov, ERROR_SUCCESS);
    }
    memset(&g_search, 0, sizeof(g_search));
    g_search.active = true;
    g_search.wantResults = numResults;
    g_search.wantType = g_search.wantMode = 0xFFFFFFFF;
    for (WORD i = 0; contexts && i < numContexts; i++) {
        if (contexts[i].dwContextId == kContextGameType) g_search.wantType = contexts[i].dwValue;
        if (contexts[i].dwContextId == kContextGameMode) g_search.wantMode = contexts[i].dwValue;
    }
    g_search.cbResults = cbResults;
    g_search.results = results;
    g_search.ov = ov;
    if (ov) ov->InternalLow = ERROR_IO_PENDING;
    else g_search.syncDone = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HANDLE syncDone = g_search.syncDone;
    Unlock();

    OverrideLog("session search: procedure %lu, want %lu, %u props, %u contexts, type %lX mode %lX", procedureIndex,
                numResults, numProperties, numContexts, g_search.wantType, g_search.wantMode);
    steam->mm->AddRequestLobbyListStringFilter(kLobbyTag, kLobbyVersion, kLobbyCompareEqual);
    steam->mm->AddRequestLobbyListDistanceFilter(kLobbyDistanceWorldwide);
    steam->mm->AddRequestLobbyListResultCountFilter(kMaxFound);
    SteamAsync(steam->mm->RequestLobbyList(), LobbyMatchList_t::k_iCallback, sizeof(LobbyMatchList_t), OnLobbyList,
               nullptr);

    if (ov) return ERROR_IO_PENDING;
    WaitForSingleObject(syncDone, kSyncSearchTimeoutMs);
    Lock();
    g_search.active = false;
    g_search.syncDone = nullptr;
    Unlock();
    CloseHandle(syncDone);
    return ERROR_SUCCESS;
}

// ---- overlapped results and handles ----

static DWORD WINAPI My_XGetOverlappedResult(XOVERLAPPED* ov, DWORD* result, BOOL wait) {
    if (!ov) return ERROR_INVALID_PARAMETER;
    if (ov->InternalLow == ERROR_IO_PENDING) {
        if (!wait) return ERROR_IO_INCOMPLETE;
        if (ov->hEvent) WaitForSingleObject(ov->hEvent, INFINITE);
        while (ov->InternalLow == ERROR_IO_PENDING) Sleep(1);
    }
    if (result) *result = (DWORD)ov->InternalHigh;
    return (DWORD)ov->InternalLow;
}

static DWORD WINAPI My_XGetOverlappedExtendedError(XOVERLAPPED* ov) {
    if (!ov) return HRESULT_FROM_WIN32(GetLastError());
    if (ov->InternalLow == ERROR_IO_PENDING) return HRESULT_FROM_WIN32(ERROR_IO_INCOMPLETE);
    return ov->dwExtendedError ? ov->dwExtendedError : HRESULT_FROM_WIN32((DWORD)ov->InternalLow);
}

static BOOL WINAPI My_XCloseHandle(HANDLE h) {
    if (InvitesCloseHandle(h)) return TRUE;
    Lock();
    Session* s = Find(h);
    if (s) {
        LeaveLobbyOf(*s);
        OverrideLog("session %p closed", h);
        CloseHandle(s->handle);
        memset(s, 0, sizeof(*s));
    }
    Unlock();
    if (s) UpdatePresence();
    return s ? TRUE : FALSE;  // other handles: the old stub closed nothing and returned 0
}

const Override kSessionOverrides[] = {
    {1082, (void*)My_XGetOverlappedExtendedError},
    {1083, (void*)My_XGetOverlappedResult},
    {5251, (void*)My_XCloseHandle},
    {5276, (void*)My_XUserSetProperty},
    {5277, (void*)My_XUserSetContext},
    {5300, (void*)My_XSessionCreate},
    {5317, (void*)My_XSessionWriteStats},
    {5318, (void*)My_XSessionStart},
    {5321, (void*)My_XSessionSearch},
    {5322, (void*)My_XSessionModify},
    {5323, (void*)My_XSessionMigrateHost},
    {5325, (void*)My_XSessionLeaveLocal},
    {5326, (void*)My_XSessionJoinRemote},
    {5327, (void*)My_XSessionJoinLocal},
    {5330, (void*)My_XSessionDelete},
    {5332, (void*)My_XSessionEnd},
    {5336, (void*)My_XSessionLeaveRemote},
};
const int kSessionOverrideCount = sizeof(kSessionOverrides) / sizeof(kSessionOverrides[0]);
