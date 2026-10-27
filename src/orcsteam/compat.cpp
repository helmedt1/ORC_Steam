// Everything the old xlive-dummy stub used to answer, re-implemented with the same observable behaviour
// (return values, out-parameters, stack cleanup), so orcsteam no longer needs the stub at all.
// Behaviour was taken from the stub's code (tools/show_stub.ps1), not from the GFWL docs, where they differ.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string.h>
#include "xlive_types.h"
#include "overrides.h"

// ---- sockets: plain Winsock; VDP (254) is UDP ----
// Not optimized so each wrapper keeps its own "ret N" instead of a tail jump into ws2_32
// (some ws2_32 imports are by ordinal, which tools/audit_impl.ps1 could not size).
#pragma optimize("", off)

static const int kProtoVdp = 254;

static SOCKET WINAPI My_XSocketCreate(int af, int type, int protocol) {
    if (type == 0) type = (protocol == 0 || protocol == IPPROTO_TCP) ? SOCK_STREAM : SOCK_DGRAM;
    if (protocol == 0) protocol = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    int asked = protocol;
    if (protocol == kProtoVdp) protocol = IPPROTO_UDP;
    SOCKET s = socket(af, type, protocol);
    if (OrcsteamDiag()) {
        int err = WSAGetLastError();
        OverrideLog("socket %u created: type %d, protocol %d", (unsigned)s, type, asked);
        WSASetLastError(err);
    }
    return s;
}
static int WINAPI My_XSocketClose(SOCKET s) {
    if (OrcsteamDiag()) OverrideLog("socket %u closed", (unsigned)s);
    return closesocket(s);
}
static int WINAPI My_XSocketShutdown(SOCKET s, int how) { return shutdown(s, how); }
static int WINAPI My_XSocketIOCTLSocket(SOCKET s, long cmd, u_long* arg) { return ioctlsocket(s, cmd, arg); }
static int WINAPI My_XSocketSetSockOpt(SOCKET s, int level, int name, const char* value, int len) {
    return setsockopt(s, level, name, value, len);
}
static int WINAPI My_XSocketGetSockOpt(SOCKET s, int level, int name, char* value, int* len) {
    return getsockopt(s, level, name, value, len);
}
static int WINAPI My_XSocketGetSockName(SOCKET s, sockaddr* name, int* len) { return getsockname(s, name, len); }
static int WINAPI My_XSocketBind(SOCKET s, const sockaddr* name, int len) {
    int r = bind(s, name, len);
    if (OrcsteamDiag()) {
        int err = WSAGetLastError();
        sockaddr_in a = {};
        int alen = sizeof(a);
        getsockname(s, (sockaddr*)&a, &alen);
        OverrideLog("socket %u bound to port %u%s (error %d)", (unsigned)s, ntohs(a.sin_port), r == 0 ? "" : " FAILED",
                    r == 0 ? 0 : err);
        WSASetLastError(err);
    }
    return r;
}
static int WINAPI My_XSocketConnect(SOCKET s, const sockaddr* name, int len) { return connect(s, name, len); }
static int WINAPI My_XSocketListen(SOCKET s, int backlog) { return listen(s, backlog); }
static SOCKET WINAPI My_XSocketAccept(SOCKET s, sockaddr* addr, int* len) { return accept(s, addr, len); }
static int WINAPI My_XSocketSelect(int n, fd_set* r, fd_set* w, fd_set* e, const timeval* t) { return select(n, r, w, e, t); }
static int WINAPI My_XSocketRecv(SOCKET s, char* buf, int len, int flags) { return recv(s, buf, len, flags); }
static int WINAPI My_XSocketSend(SOCKET s, const char* buf, int len, int flags) { return send(s, buf, len, flags); }
static ULONG WINAPI My_XSocketInet_Addr(const char* text) {
    IN_ADDR a;
    return text && inet_pton(AF_INET, text, &a) == 1 ? a.s_addr : INADDR_NONE;
}

#pragma optimize("", on)

// ---- XNet / online lifetime ----

static INT WINAPI My_XNetStartup(const void* params) { return 0; }
static INT WINAPI My_XNetQosListen(const XNKID* kid, const BYTE* data, UINT size, DWORD bitsPerSec, DWORD flags) { return 0; }
static DWORD WINAPI My_XOnlineStartup() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
}
static DWORD WINAPI My_XOnlineCleanup() { return WSACleanup(); }
static DWORD WINAPI My_XOnlineGetNatType() { return 1; }  // NAT_OPEN

// ---- frame hooks and UI: nothing to draw, Steam's overlay replaces the Guide ----

static DWORD WINAPI My_XLiveInput(void* info) { return 0; }
static DWORD WINAPI My_XLiveRender() { return 0; }
static DWORD WINAPI My_XLiveOnResetDevice(void* params) { return 0; }
static DWORD WINAPI My_XLivePreTranslateMessage(const MSG* msg) { return 0; }
static DWORD WINAPI My_XShowMessageComposeUI(DWORD user, const XUID* to, DWORD count, const WCHAR* text) { return 1; }
static DWORD WINAPI My_XShowFriendRequestUI(DWORD user, XUID xuid) { return 1; }
static DWORD WINAPI My_XShowSigninUI(DWORD panes, DWORD flags) { return 0; }
static DWORD WINAPI My_XShowMarketplaceUI(DWORD user, DWORD entry, ULONGLONG offer, DWORD categories) { return 1; }
static DWORD WINAPI My_XCancelOverlapped(XOVERLAPPED* ov) { return 0; }

// ---- user data the game never gets back: achievements, stats, mute list ----

static DWORD WINAPI My_XUserWriteAchievements(DWORD count, const void* achievements, XOVERLAPPED* ov) { return 0; }
static DWORD WINAPI My_Ordinal5293(DWORD a, DWORD b, DWORD c, DWORD d, DWORD e) { return 0; }  // XUserSetPropertyEx-like, 5 args
static DWORD WINAPI My_XUserMuteListQuery(DWORD user, XUID xuid, BOOL* onList) {
    *onList = FALSE;
    return 0;
}
static DWORD WINAPI My_XUserCreateStatsEnumeratorByRank(DWORD title, DWORD rankStart, DWORD rows, DWORD numSpecs,
                                                        const void* specs, DWORD* cbBuffer, HANDLE* ph) {
    if (cbBuffer) *cbBuffer = 0;
    *ph = INVALID_HANDLE_VALUE;
    return 1;
}
static DWORD WINAPI My_XUserCreateStatsEnumeratorByXuid(DWORD title, XUID pivot, DWORD rows, DWORD numSpecs,
                                                        const void* specs, DWORD* cbBuffer, HANDLE* ph) {
    *ph = INVALID_HANDLE_VALUE;
    return 1;
}

// ---- saves: "protected" data is stored as-is (the stub copied bytes, so existing saves stay readable) ----

static DWORD WINAPI My_XLiveProtectData(const BYTE* in, DWORD cbIn, BYTE* out, DWORD* cbOut, HANDLE context) {
    if (out && *cbOut >= cbIn) {
        memcpy(out, in, cbIn);
        *cbOut = cbIn;
        return 0;
    }
    *cbOut = cbIn;
    return 0x8007007A;  // HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)
}
static DWORD WINAPI My_XLiveCreateProtectedDataContext(const void* info, HANDLE* context) {
    if (context) *context = (HANDLE)1;
    return 0;
}
static DWORD WINAPI My_XLiveCloseProtectedDataContext(HANDLE context) { return 0; }

// ---- profile settings: defaults for everything, title-specific blobs have no value ----

#pragma pack(push, 8)
struct XUSER_PROFILE_SETTING {
    DWORD source;
    DWORD pad;
    union { DWORD dwUserIndex; XUID xuid; } user;
    DWORD dwSettingId;
    DWORD pad2;
    BYTE type;
    BYTE pad3[7];
    union { LONG nData; LONGLONG i64Data; double dblData; struct { DWORD cbData; void* pbData; } blob; } value;
};
#pragma pack(pop)
static_assert(sizeof(XUSER_PROFILE_SETTING) == 0x28, "the stub laid settings out every 0x28 bytes");

static const DWORD kSourceNoValue = 0, kSourceDefault = 1;
static const BYTE kTypeUnicode = 4, kTypeBinary = 6, kTypeNull = 0xFF;
static const DWORD kFirstTitleSpecific = 0x3FFD;

static DWORD WINAPI My_XUserReadProfileSettings(DWORD title, DWORD userIndex, DWORD count, const DWORD* ids,
                                                DWORD* cbResults, BYTE* results, XOVERLAPPED* ov) {
    DWORD need = 8 + count * sizeof(XUSER_PROFILE_SETTING);
    for (DWORD i = 0; i < count; i++) {
        BYTE type = (BYTE)(ids[i] >> 28);
        if (type == kTypeUnicode || type == kTypeBinary) need += (ids[i] >> 16) & 0xFFF;
    }
    if (!results || *cbResults < need) {
        *cbResults = need;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    *cbResults = need;
    memset(results, 0, need);
    XUSER_PROFILE_SETTING* settings = (XUSER_PROFILE_SETTING*)(results + 8);
    *(DWORD*)results = count;
    *(XUSER_PROFILE_SETTING**)(results + 4) = settings;
    BYTE* extra = (BYTE*)(settings + count);
    for (DWORD i = 0; i < count; i++) {
        XUSER_PROFILE_SETTING& s = settings[i];
        DWORD id = ids[i];
        BYTE type = (BYTE)(id >> 28);
        DWORD size = (id >> 16) & 0xFFF;
        s.user.dwUserIndex = userIndex;
        s.dwSettingId = id;
        if ((id & 0x3FFF) >= kFirstTitleSpecific) {  // the stub looked for files it never wrote
            s.source = kSourceNoValue;
            s.type = kTypeNull;
            continue;
        }
        s.source = kSourceDefault;
        s.type = type == 0xF ? kTypeNull : type;
        if (type == kTypeUnicode) {
            s.value.blob.pbData = extra;
            s.value.blob.cbData = 2;  // empty string
            extra += size;
        } else if (type == kTypeBinary) {
            s.value.blob.pbData = extra;
            s.value.blob.cbData = size;
            extra += size;
        }  // numbers, floats and dates stay zero
    }
    return 0;  // synchronous, overlapped untouched (like the stub)
}

static DWORD WINAPI My_XUserReadProfileSettingsByXuid(DWORD title, DWORD userIndex, DWORD numXuids, const XUID* xuids,
                                                      DWORD count, const DWORD* ids, DWORD* cbResults, void* results,
                                                      XOVERLAPPED* ov) {
    return ERROR_NOT_FOUND;
}

// Voice (XHVCreateEngine) lives in voice.cpp.

const Override kCompatOverrides[] = {
    {3, (void*)My_XSocketCreate},
    {4, (void*)My_XSocketClose},
    {5, (void*)My_XSocketShutdown},
    {6, (void*)My_XSocketIOCTLSocket},
    {7, (void*)My_XSocketSetSockOpt},
    {8, (void*)My_XSocketGetSockOpt},
    {9, (void*)My_XSocketGetSockName},
    {11, (void*)My_XSocketBind},
    {12, (void*)My_XSocketConnect},
    {13, (void*)My_XSocketListen},
    {14, (void*)My_XSocketAccept},
    {15, (void*)My_XSocketSelect},
    {18, (void*)My_XSocketRecv},
    {22, (void*)My_XSocketSend},
    {26, (void*)My_XSocketInet_Addr},
    {51, (void*)My_XNetStartup},
    {69, (void*)My_XNetQosListen},
    {5001, (void*)My_XLiveInput},
    {5002, (void*)My_XLiveRender},
    {5007, (void*)My_XLiveOnResetDevice},
    {5030, (void*)My_XLivePreTranslateMessage},
    {5034, (void*)My_XLiveProtectData},
    {5035, (void*)My_XLiveProtectData},  // unprotect is the same copy
    {5036, (void*)My_XLiveCreateProtectedDataContext},
    {5038, (void*)My_XLiveCloseProtectedDataContext},
    {5209, (void*)My_XShowMessageComposeUI},
    {5210, (void*)My_XShowFriendRequestUI},
    {5254, (void*)My_XCancelOverlapped},
    {5260, (void*)My_XShowSigninUI},
    {5278, (void*)My_XUserWriteAchievements},
    {5284, (void*)My_XUserCreateStatsEnumeratorByRank},
    {5286, (void*)My_XUserCreateStatsEnumeratorByXuid},
    {5293, (void*)My_Ordinal5293},
    {5310, (void*)My_XOnlineStartup},
    {5311, (void*)My_XOnlineCleanup},
    {5314, (void*)My_XUserMuteListQuery},
    {5324, (void*)My_XOnlineGetNatType},
    {5331, (void*)My_XUserReadProfileSettings},
    {5339, (void*)My_XUserReadProfileSettingsByXuid},
    {5365, (void*)My_XShowMarketplaceUI},
};
const int kCompatOverrideCount = sizeof(kCompatOverrides) / sizeof(kCompatOverrides[0]);
