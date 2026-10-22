// Stage 4: Steam invites and "Join game" from the Steam friends list.
// Joining side: Steam reports GameLobbyJoinRequested (invite accepted / Join clicked / +connect_lobby on launch).
// We enter that lobby, read the host's XSESSION_INFO from it and raise GFWL's own XN_LIVE_INVITE_ACCEPTED;
// the game then calls XInviteGetAcceptedInfo and joins exactly as it did with GFWL.
// Hosting side: the game's friend list (XFriendsCreateEnumerator) lists Steam friends, XInviteSend sends Steam
// lobby invites, and the Guide/gamer card open the Steam overlay.

#define WIN32_LEAN_AND_MEAN
#include "xlive_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "overrides.h"
#include "steam.h"

static const DWORD kNotifyInviteAccepted = 0x02000002;  // XN_LIVE_INVITE_ACCEPTED, checked by the exe at 0x49BDEC
static const ULONGLONG kAreaLive = 0x2;                  // XNOTIFY_LIVE
static const DWORD kTitleId = 0x43430FA1;
static const XUID kOnlineXuidBase = 0x0009000000000000ull;
static const DWORD kAppId = 209100;
static const int kMaxListeners = 16;
static const int kMaxEnumerators = 4;
static const int kMaxFriends = 100;
static const DWORD kInviteLobbyTimeoutMs = 120000;  // leave an invite lobby the game never used
static const DWORD kLaunchInviteDelayMs = 20000;    // +connect_lobby: give the game time to reach its menus

// The game was built with these packed to 4: it reserves 84 bytes for XINVITE_INFO (stack frame at 0xBCE3A0,
// GS cookie right after) and walks XONLINE_FRIEND in steps of 0xC4 (0xACA754). Writing 8-aligned sizes
// overflowed the invite buffer and killed the game through its stack cookie check.
#pragma pack(push, 4)
struct XINVITE_INFO {
    XUID xuidInvitee;
    XUID xuidInviter;
    DWORD dwTitleID;
    XSESSION_INFO hostInfo;
    BOOL fFromGameInvite;
};

struct XONLINE_FRIEND {
    XUID xuid;
    CHAR szGamertag[16];
    DWORD dwFriendState;
    XNKID sessionID;
    DWORD dwTitleID;
    FILETIME ftUserTime;
    XNKID xnkidInvite;
    FILETIME gameinviteTime;
    DWORD cchRichPresence;
    WCHAR wszRichPresence[64];
};
#pragma pack(pop)
static_assert(sizeof(XINVITE_INFO) == 84, "XINVITE_INFO must match the game's 84-byte buffer");
static_assert(sizeof(XONLINE_FRIEND) == 0xC4, "XONLINE_FRIEND must match the game's 0xC4 stride");
static const DWORD kFriendOnline = 0x1, kFriendPlaying = 0x2, kFriendJoinable = 0x10;

struct Listener { HANDLE handle; ULONGLONG areas; bool invitePending; };
struct Enumerator { HANDLE handle; int count, next; XONLINE_FRIEND* items; };

struct Invite {
    uint64 lobby;      // lobby we are entering / entered because of the invite
    bool ready;        // lobby entered and session info read
    bool notified;     // XN_LIVE_INVITE_ACCEPTED raised
    DWORD readyTick;
    XUID inviter;
    XSESSION_INFO info;
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static Listener g_listeners[kMaxListeners];
static Enumerator g_enums[kMaxEnumerators];
static Invite g_invite;
static uint64 g_launchLobby;  // from +connect_lobby, joined after kLaunchInviteDelayMs
static DWORD g_steamReadyTick;

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

// ---- notifications ----
// Like the old stub: one global queue per notification area, taken by whichever listener asks first,
// and XLiveInitializeEx posts XN_SYS_SIGNINCHANGED. Invite acceptance is delivered to every LIVE listener.

static const DWORD kNotifySigninChanged = 0x0000000A;  // XN_SYS_SIGNINCHANGED
static const int kMaxPending = 32;
struct Pending { DWORD id; ULONG_PTR param; };
static Pending g_pending[kMaxPending];
static int g_pendingCount;

static int AreaOf(DWORD id) { return (id >> 25) & 0x3F; }

static void PostNotification(DWORD id, ULONG_PTR param) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_pendingCount < kMaxPending) g_pending[g_pendingCount++] = {id, param};
    for (Listener& l : g_listeners)
        if (l.handle && (l.areas & (1ull << AreaOf(id)))) SetEvent(l.handle);
    ReleaseSRWLockExclusive(&g_lock);
}

static DWORD WINAPI My_XLiveInitializeEx(const void* info, DWORD version) {
    PostNotification(kNotifySigninChanged, 1);  // user 0 signed in
    return 0;
}

static void RaiseInviteAccepted() {
    int n = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (Listener& l : g_listeners)
        if (l.handle && (l.areas & kAreaLive)) {
            l.invitePending = true;
            SetEvent(l.handle);  // listener handles are events the game may wait on
            n++;
        }
    g_invite.notified = true;
    ReleaseSRWLockExclusive(&g_lock);
    OverrideLog("invite: XN_LIVE_INVITE_ACCEPTED raised for %d listener(s)", n);
}

static HANDLE WINAPI My_XNotifyCreateListener(ULONGLONG areas) {
    HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!h) return nullptr;
    AcquireSRWLockExclusive(&g_lock);
    for (Listener& l : g_listeners)
        if (!l.handle) {
            l = {h, areas, false};
            break;
        }
    for (int i = 0; i < g_pendingCount; i++)
        if (areas & (1ull << AreaOf(g_pending[i].id))) SetEvent(h);
    ReleaseSRWLockExclusive(&g_lock);
    return h;
}

static BOOL WINAPI My_XNotifyGetNext(HANDLE h, DWORD filter, DWORD* id, ULONG_PTR* param) {
    if (!h || h == INVALID_HANDLE_VALUE || !id) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *id = 0;
    if (param) *param = 0;
    bool invite = false, found = false;
    Pending got = {};
    AcquireSRWLockExclusive(&g_lock);
    for (Listener& l : g_listeners)
        if (l.handle == h && l.invitePending && (filter == 0 || filter == kNotifyInviteAccepted)) {
            l.invitePending = false;
            invite = true;
        }
    if (!invite) {
        int pick = -1;
        if (filter) {
            for (int i = 0; i < g_pendingCount && pick < 0; i++)
                if (g_pending[i].id == filter) pick = i;
        } else {
            for (int area = 0; area < 8 && pick < 0; area++)
                for (int i = 0; i < g_pendingCount && pick < 0; i++)
                    if (AreaOf(g_pending[i].id) == area) pick = i;
        }
        if (pick >= 0) {
            got = g_pending[pick];
            memmove(&g_pending[pick], &g_pending[pick + 1], (g_pendingCount - pick - 1) * sizeof(Pending));
            g_pendingCount--;
            found = true;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (invite) {
        *id = kNotifyInviteAccepted;  // param: user index 0
        OverrideLog("invite: notification delivered to listener %p", h);
        return TRUE;
    }
    if (found) {
        *id = got.id;
        if (param) *param = got.param;
        OverrideLog("notification %08lX (param %lu) delivered", got.id, (DWORD)got.param);
        return TRUE;
    }
    ResetEvent(h);
    return FALSE;
}

static DWORD WINAPI My_XInviteGetAcceptedInfo(DWORD userIndex, XINVITE_INFO* info) {
    AcquireSRWLockShared(&g_lock);
    bool have = g_invite.ready;
    Invite inv = g_invite;
    ReleaseSRWLockShared(&g_lock);
    if (!have) return 1;  // no invite (the old stub's answer)
    if (!info) return ERROR_INVALID_PARAMETER;
    memset(info, 0, sizeof(*info));
    info->xuidInvitee = LocalXuid();
    info->xuidInviter = inv.inviter;
    info->dwTitleID = kTitleId;
    info->hostInfo = inv.info;
    info->fFromGameInvite = TRUE;
    OverrideLog("invite: accepted info handed to the game (host account %lu)", XnAddrAccountId(&inv.info.hostAddress));
    return ERROR_SUCCESS;
}

// ---- joining side ----

static void OnInviteLobbyEntered(void* ctx, const void* result, bool failed) {
    const LobbyEnter_t* r = (const LobbyEnter_t*)result;
    const SteamApi* steam = Steam();
    uint64 lobby = (uint64)(ULONG_PTR)ctx;  // low 32 bits only; full id comes from the result
    if (failed || !r || r->m_EChatRoomEnterResponse != 1 || !steam) {
        OverrideLog("!! invite: could not enter lobby (io %d, response %lu)", failed, r ? r->m_EChatRoomEnterResponse : 0);
        AcquireSRWLockExclusive(&g_lock);
        g_invite.lobby = 0;
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    lobby = r->m_ulSteamIDLobby;
    CSteamID id(lobby);
    XSESSION_INFO info;
    if (!steam->mm->GetLobbyData(id, "orcsteam")[0] || !Unhex(steam->mm->GetLobbyData(id, "info"), &info, sizeof(info))) {
        OverrideLog("!! invite: lobby %016llX is not an orcsteam game", lobby);
        steam->mm->LeaveLobby(id);
        AcquireSRWLockExclusive(&g_lock);
        g_invite.lobby = 0;
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    DWORD hostAccount = XnAddrAccountId(&info.hostAddress);
    RegisterFoundLobby(info.sessionID, lobby);
    NetAcceptPeer(hostAccount);
    AcquireSRWLockExclusive(&g_lock);
    g_invite.lobby = lobby;
    g_invite.info = info;
    g_invite.inviter = kOnlineXuidBase | hostAccount;
    g_invite.ready = true;
    g_invite.notified = false;
    g_invite.readyTick = GetTickCount();
    ReleaseSRWLockExclusive(&g_lock);
    OverrideLog("invite: entered lobby %016llX, host account %lu", lobby, hostAccount);
    RaiseInviteAccepted();
}

static void JoinFromSteam(uint64 lobby, const char* how) {
    const SteamApi* steam = Steam();
    if (!steam || !lobby) return;
    if (lobby == HostedLobby() || LobbyInUse(lobby)) {
        OverrideLog("invite: %s for lobby %016llX ignored, already in it", how, lobby);
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    uint64 previous = g_invite.ready && !LobbyInUse(g_invite.lobby) ? g_invite.lobby : 0;
    memset(&g_invite, 0, sizeof(g_invite));
    g_invite.lobby = lobby;
    ReleaseSRWLockExclusive(&g_lock);
    if (previous) steam->mm->LeaveLobby(CSteamID(previous));
    OverrideLog("invite: %s, entering lobby %016llX", how, lobby);
    SteamAsync(steam->mm->JoinLobby(CSteamID(lobby)), LobbyEnter_t::k_iCallback, sizeof(LobbyEnter_t),
               OnInviteLobbyEntered, nullptr);
}

class JoinRequestCallback : public CCallbackBase {
public:
    void Run(void* param) override {
        const GameLobbyJoinRequested_t* req = (const GameLobbyJoinRequested_t*)param;
        JoinFromSteam(req->m_steamIDLobby.v, "steam join request");
    }
    void Run(void* param, bool, SteamAPICall_t) override { Run(param); }
    int GetCallbackSizeBytes() override { return sizeof(GameLobbyJoinRequested_t); }
};
static JoinRequestCallback g_joinRequest;

void InvitesOnSteamReady(const SteamApi& steam) {
    g_steamReadyTick = GetTickCount();
    SteamRegisterCallback(&g_joinRequest, GameLobbyJoinRequested_t::k_iCallback);
    // Steam starts the game with "+connect_lobby <id>" when an invite is accepted while it is not running.
    const char* cmd = strstr(GetCommandLineA(), "+connect_lobby");
    if (cmd) {
        g_launchLobby = _strtoui64(cmd + strlen("+connect_lobby"), nullptr, 10);
        OverrideLog("invite: launched with +connect_lobby %016llX, joining in %lu s", g_launchLobby,
                    kLaunchInviteDelayMs / 1000);
    }
}

void InvitesPump(const SteamApi& steam) {
    // Self-test hook: Steam cannot deliver a join request to the lobby owner, so the test sets this instead.
    char testLobby[32];
    if (GetEnvironmentVariableA("ORCSTEAM_TEST_JOIN", testLobby, sizeof(testLobby))) {
        SetEnvironmentVariableA("ORCSTEAM_TEST_JOIN", nullptr);
        JoinFromSteam(_strtoui64(testLobby, nullptr, 10), "test join request");
    }
    if (g_launchLobby && GetTickCount() - g_steamReadyTick >= kLaunchInviteDelayMs) {
        uint64 lobby = g_launchLobby;
        g_launchLobby = 0;
        JoinFromSteam(lobby, "launch invite");
    }
    // Leave an invite lobby the game never turned into a session (invite ignored or declined in-game).
    AcquireSRWLockExclusive(&g_lock);
    uint64 stale = 0;
    if (g_invite.ready && GetTickCount() - g_invite.readyTick >= kInviteLobbyTimeoutMs) {
        stale = g_invite.lobby;
        memset(&g_invite, 0, sizeof(g_invite));
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (stale && !LobbyInUse(stale)) {
        steam.mm->LeaveLobby(CSteamID(stale));
        OverrideLog("invite: lobby %016llX unused by the game, left", stale);
    }
}

// ---- hosting side: friends list, invites, overlay ----

static DWORD WINAPI My_XFriendsCreateEnumerator(DWORD userIndex, DWORD start, DWORD count, DWORD* cbBuffer, HANDLE* ph) {
    const SteamApi* steam = Steam();
    if (!steam || userIndex != 0) {  // no friends to list
        if (ph) *ph = INVALID_HANDLE_VALUE;
        return 0;
    }
    if (!cbBuffer || !ph || !count || count > (DWORD)kMaxFriends) return ERROR_INVALID_PARAMETER;

    // Online Steam friends, the ones playing ORC first.
    XONLINE_FRIEND* items = (XONLINE_FRIEND*)calloc(kMaxFriends, sizeof(XONLINE_FRIEND));
    if (!items) return ERROR_NOT_ENOUGH_MEMORY;
    int total = steam->friends->GetFriendCount(kFriendFlagImmediate), n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < total && n < kMaxFriends; i++) {
            CSteamID id = steam->friends->GetFriendByIndex(i, kFriendFlagImmediate);
            if (steam->friends->GetFriendPersonaState(id) == kPersonaOffline) continue;
            FriendGameInfo_t game = {};
            bool inOrc = steam->friends->GetFriendGamePlayed(id, &game) && (DWORD)(game.m_gameID & 0xFFFFFF) == kAppId;
            if (inOrc != (pass == 0)) continue;
            XONLINE_FRIEND& f = items[n++];
            f.xuid = kOnlineXuidBase | id.AccountId();
            SteamNameToGamertag(steam->friends->GetFriendPersonaName(id), f.szGamertag, sizeof(f.szGamertag));
            f.dwFriendState = kFriendOnline | (inOrc ? kFriendPlaying : 0) | (inOrc && game.m_steamIDLobby.v ? kFriendJoinable : 0);
            f.dwTitleID = inOrc ? kTitleId : 0;
        }

    AcquireSRWLockExclusive(&g_lock);
    Enumerator* e = nullptr;
    for (Enumerator& c : g_enums)
        if (!c.handle) { e = &c; break; }
    if (e) {
        e->handle = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        e->items = items;
        e->count = n;
        e->next = (int)start < n ? (int)start : n;
        *ph = e->handle;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!e) {
        free(items);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    *cbBuffer = count * sizeof(XONLINE_FRIEND);
    OverrideLog("friends: %d online steam friend(s) listed for the game", n);
    return ERROR_SUCCESS;
}

static DWORD WINAPI My_XEnumerate(HANDLE h, void* buffer, DWORD cbBuffer, DWORD* itemsReturned, XOVERLAPPED* ov) {
    AcquireSRWLockExclusive(&g_lock);
    Enumerator* e = nullptr;
    for (Enumerator& c : g_enums)
        if (c.handle && c.handle == h) e = &c;
    DWORD copied = 0;
    if (e && buffer) {
        DWORD fit = cbBuffer / sizeof(XONLINE_FRIEND);
        while (copied < fit && e->next < e->count) {
            ((XONLINE_FRIEND*)buffer)[copied++] = e->items[e->next++];
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!e) {  // any other enumerator (stats, etc.) is always empty
        if (itemsReturned) *itemsReturned = 0;
        return ERROR_NO_MORE_FILES;
    }
    if (itemsReturned) *itemsReturned = copied;
    return CompleteOverlapped(ov, copied ? ERROR_SUCCESS : ERROR_NO_MORE_FILES, copied);
}

bool InvitesCloseHandle(void* h) {
    AcquireSRWLockExclusive(&g_lock);
    for (Enumerator& e : g_enums)
        if (e.handle && e.handle == h) {
            CloseHandle(e.handle);
            free(e.items);
            memset(&e, 0, sizeof(e));
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
    for (Listener& l : g_listeners)
        if (l.handle && l.handle == h) {
            CloseHandle(l.handle);
            memset(&l, 0, sizeof(l));
            ReleaseSRWLockExclusive(&g_lock);
            return true;
        }
    ReleaseSRWLockExclusive(&g_lock);
    return false;
}

// The stub's #5316 pops no arguments (needs 20 bytes). Ours turns game invites into Steam lobby invites.
static DWORD WINAPI My_XInviteSend(DWORD userIndex, DWORD count, const XUID* xuids, const WCHAR* text, XOVERLAPPED* ov) {
    const SteamApi* steam = Steam();
    uint64 lobby = HostedLobby();
    int sent = 0;
    if (steam && lobby && xuids)
        for (DWORD i = 0; i < count; i++)
            if (steam->mm->InviteUserToLobby(CSteamID(lobby), CSteamID::Individual((DWORD)(xuids[i] & 0xFFFFFFFF)))) sent++;
    OverrideLog("invite: %d of %lu steam invite(s) sent%s", sent, count, lobby ? "" : " (no hosted lobby yet)");
    return CompleteOverlapped(ov, ERROR_SUCCESS, 0);
}

// Guide button: the Steam overlay's invite dialog while hosting, its friends page otherwise.
static DWORD WINAPI My_XShowGuideUI(DWORD userIndex) {
    const SteamApi* steam = Steam();
    if (!steam) return ERROR_SUCCESS;
    uint64 lobby = HostedLobby();
    if (lobby) steam->friends->ActivateGameOverlayInviteDialog(CSteamID(lobby));
    else steam->friends->ActivateGameOverlay("Friends");
    return ERROR_SUCCESS;
}

static DWORD WINAPI My_XShowGamerCardUI(DWORD userIndex, XUID xuid) {
    if (const SteamApi* steam = Steam())
        steam->friends->ActivateGameOverlayToUser("steamid", CSteamID::Individual((DWORD)(xuid & 0xFFFFFFFF)));
    return ERROR_SUCCESS;
}

const Override kInviteOverrides[] = {
    {651, (void*)My_XNotifyGetNext},
    {5215, (void*)My_XShowGuideUI},
    {5252, (void*)My_XShowGamerCardUI},
    {5256, (void*)My_XEnumerate},
    {5270, (void*)My_XNotifyCreateListener},
    {5312, (void*)My_XFriendsCreateEnumerator},
    {5315, (void*)My_XInviteGetAcceptedInfo},
    {5316, (void*)My_XInviteSend},
    {5297, (void*)My_XLiveInitializeEx},
};
const int kInviteOverrideCount = sizeof(kInviteOverrides) / sizeof(kInviteOverrides[0]);
