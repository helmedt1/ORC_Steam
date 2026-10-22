// Stage 2a: our own implementations of the sign-in functions, identity taken from Steam.
// Everything not listed in kOverrides still goes to the original stub.

#define WIN32_LEAN_AND_MEAN
#include "xlive_types.h"
#include <stdlib.h>
#include <string.h>
#include "overrides.h"
#include "steam.h"

static const DWORD kSignedInToLive = 2;
static const DWORD kInfoLiveEnabled = 0x1;
static const DWORD kGetSigninInfoOfflineXuidOnly = 0x1;
static const DWORD kXnAddrOnlineFlags = 0x02 | 0x04 | 0x20 | 0x40 | 0x80;  // ETHERNET|STATIC|GATEWAY|DNS|ONLINE
static const XUID kOnlineXuidBase = 0x0009000000000000ull;
static const XUID kOfflineXuidBase = 0xE000000000000000ull;

struct Identity {
    bool ready;
    DWORD accountId;
    char name[16];  // ASCII: GFWL gamertags were ASCII and ORC's font has no glyphs for anything else
};
static Identity g_id;

// Steam persona (UTF-8) -> gamertag-style ASCII, at most 15 chars. Cyrillic is transliterated,
// other scripts go through Windows' best-fit ASCII mapping (é -> e), the rest becomes '_'.
void SteamNameToGamertag(const char* utf8, char* out, unsigned int cap) {
    static const char* const kCyrillic[64] = {
        "A", "B", "V", "G", "D", "E", "Zh", "Z", "I", "Y", "K", "L", "M", "N", "O", "P",
        "R", "S", "T", "U", "F", "Kh", "Ts", "Ch", "Sh", "Shch", "", "Y", "", "E", "Yu", "Ya",
        "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
        "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "", "y", "", "e", "yu", "ya"};
    WCHAR wide[64];
    size_t n = 0;
    if (utf8 && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 64)) {
        for (const WCHAR* w = wide; *w && n + 1 < cap; w++) {
            char piece[8] = {};
            if (*w >= 0x20 && *w < 0x7F) piece[0] = (char)*w;
            else if (*w >= 0x0410 && *w <= 0x044F) strcpy_s(piece, kCyrillic[*w - 0x0410]);
            else if (*w == 0x0401) strcpy_s(piece, "Yo");
            else if (*w == 0x0451) strcpy_s(piece, "yo");
            else if (!WideCharToMultiByte(20127, 0, w, 1, piece, 1, "_", nullptr) || piece[0] < 0x20) piece[0] = '_';
            for (const char* p = piece; *p && n + 1 < cap; p++) out[n++] = *p;
        }
    }
    out[n] = 0;
    if (!n) strcpy_s(out, cap, "Player");
}

static const Identity& GetIdentity() {
    if (g_id.ready) return g_id;
    const SteamApi* s = Steam();
    if (!s) {
        // Steam not initialised yet: answer with a placeholder, retry on the next call.
        static Identity placeholder = {false, 1, "Player"};
        return placeholder;
    }
    g_id.accountId = s->user->GetSteamID().AccountId();
    SteamNameToGamertag(s->friends->GetPersonaName(), g_id.name, sizeof(g_id.name));
    g_id.ready = true;
    OverrideLog("steam identity: account %lu, name '%s'", g_id.accountId, g_id.name);
    return g_id;
}

XUID LocalXuid() {
    return kOnlineXuidBase | GetIdentity().accountId;
}

DWORD LocalAccountId() {
    return GetIdentity().accountId;
}

const char* LocalGamertag() {
    return GetIdentity().name;
}

// Virtual LAN address of a player: 10.x.y.z from the low 24 bits of the Steam account id.
// abOnline carries the full account id so any peer can map the address back to a SteamID.
void FillXnAddrFor(DWORD accountId, XNADDR* addr) {
    memset(addr, 0, sizeof(*addr));
    addr->ina.S_un.S_addr = VirtualIpFor(accountId);
    addr->inaOnline = addr->ina;
    addr->wPortOnline = _byteswap_ushort(1000);
    memcpy(addr->abEnet, "\x00\x50\xF2", 3);  // locally administered MAC from the account id
    memcpy(addr->abEnet + 3, &accountId, 3);
    memcpy(addr->abOnline, &accountId, sizeof(accountId));
}

void FillLocalXnAddr(XNADDR* addr) {
    FillXnAddrFor(GetIdentity().accountId, addr);
}

DWORD VirtualIpFor(DWORD accountId) {
    return _byteswap_ulong(0x0A000000 | (accountId & 0x00FFFFFF));
}

DWORD XnAddrAccountId(const XNADDR* addr) {
    DWORD id;
    memcpy(&id, addr->abOnline, sizeof(id));
    return id;
}

static DWORD WINAPI My_XUserGetSigninState(DWORD userIndex) {
    return userIndex == 0 ? kSignedInToLive : 0;
}

static DWORD WINAPI My_XUserGetSigninInfo(DWORD userIndex, DWORD flags, XUSER_SIGNIN_INFO* info) {
    if (!info) return ERROR_INVALID_PARAMETER;
    if (userIndex != 0) return ERROR_NO_SUCH_USER;
    const Identity& id = GetIdentity();
    memset(info, 0, sizeof(*info));
    info->xuid = ((flags & kGetSigninInfoOfflineXuidOnly) ? kOfflineXuidBase : kOnlineXuidBase) | id.accountId;
    info->dwInfoFlags = kInfoLiveEnabled;
    info->UserSigninState = kSignedInToLive;
    strncpy_s(info->szUserName, id.name, _TRUNCATE);
    return ERROR_SUCCESS;
}

static DWORD WINAPI My_XUserGetName(DWORD userIndex, LPSTR name, DWORD cchName) {
    if (userIndex != 0) return ERROR_NO_SUCH_USER;
    if (!name || !cchName) return ERROR_INVALID_PARAMETER;
    strncpy_s(name, cchName, GetIdentity().name, _TRUNCATE);
    return ERROR_SUCCESS;
}

static DWORD WINAPI My_XUserCheckPrivilege(DWORD userIndex, DWORD privilege, BOOL* result) {
    if (!result) return ERROR_INVALID_PARAMETER;
    *result = userIndex == 0;
    return userIndex == 0 ? ERROR_SUCCESS : ERROR_NO_SUCH_USER;
}

static DWORD WINAPI My_XNetGetTitleXnAddr(XNADDR* addr) {
    if (addr) FillLocalXnAddr(addr);
    return kXnAddrOnlineFlags;
}

// Stop our Steam pump before the game shuts Steam down.
static DWORD WINAPI My_XLiveUninitialize() {
    SteamStopPump();
    return 0;
}

const Override kOverrides[] = {
    {73, (void*)My_XNetGetTitleXnAddr},
    {5003, (void*)My_XLiveUninitialize},
    {5262, (void*)My_XUserGetSigninState},
    {5263, (void*)My_XUserGetName},
    {5265, (void*)My_XUserCheckPrivilege},
    {5267, (void*)My_XUserGetSigninInfo},
};
const int kOverrideCount = sizeof(kOverrides) / sizeof(kOverrides[0]);
