#pragma once

struct Override { unsigned ord; void* fn; };

// Sign-in / identity (overrides.cpp)
extern const Override kOverrides[];
extern const int kOverrideCount;

// Sessions, overlapped results, handles (sessions.cpp)
extern const Override kSessionOverrides[];
extern const int kSessionOverrideCount;

// Everything the old xlive-dummy stub used to answer (compat.cpp)
extern const Override kCompatOverrides[];
extern const int kCompatOverrideCount;

void OverrideLog(const char* fmt, ...);

// [orcsteam] settings from orcsteam.ini next to the DLL (core.cpp); the file is optional.
int OrcsteamIniInt(const char* key, int fallback);
void OrcsteamIniString(const char* key, const char* fallback, char* out, unsigned long cap);
bool OrcsteamDiag();  // diag=1 in orcsteam.ini (or ORCSTEAM_DIAG=1): detailed network log for test builds

// Transport: sockets, XNet addresses, QoS (net.cpp)
extern const Override kNetOverrides[];
extern const int kNetOverrideCount;

void NetAcceptPeer(unsigned long accountId);  // remember a peer's address, allow its P2P traffic

struct XNKID;
struct XOVERLAPPED;
void ActiveSessionKid(XNKID* kid);  // sessions.cpp; zeros if none
unsigned long CompleteOverlapped(XOVERLAPPED* ov, unsigned long result, unsigned long value);
unsigned long long HostedLobby();   // Steam lobby of the session we host, 0 if none
bool LobbyInUse(unsigned long long lobby);
void RegisterFoundLobby(const XNKID& kid, unsigned long long lobby);

// Invites, friends, notifications (invites.cpp)
extern const Override kInviteOverrides[];
extern const int kInviteOverrideCount;
bool InvitesCloseHandle(void* h);  // true if h was one of ours (friends enumerator)
void SteamNameToGamertag(const char* utf8, char* out, unsigned int cap);

// Identity and virtual addresses (overrides.cpp)
struct XNADDR;
unsigned long long LocalXuid();
unsigned long LocalAccountId();
const char* LocalGamertag();  // ASCII, what the game shows as our name
void FillLocalXnAddr(XNADDR* addr);
void FillXnAddrFor(unsigned long accountId, XNADDR* addr);
unsigned long VirtualIpFor(unsigned long accountId);  // network byte order
unsigned long XnAddrAccountId(const XNADDR* addr);
