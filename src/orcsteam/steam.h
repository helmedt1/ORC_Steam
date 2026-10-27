#pragma once
// Old Steamworks interfaces exported by the game's own steam_api.dll (2012):
// SteamUser016, SteamFriends009, SteamMatchMaking008, SteamNetworking005, SteamUtils005.
// Method order verified against open-steamworks headers. Declared in full up to the last
// method we use, so MSVC builds the same vtable layout Valve's MSVC build has.

#include <windows.h>

typedef unsigned long long uint64;
typedef unsigned long long SteamAPICall_t;

struct CSteamID {
    uint64 v;
    CSteamID() : v(0) {}  // user ctor => returned via hidden pointer, same as Valve's build
    explicit CSteamID(uint64 x) : v(x) {}
    DWORD AccountId() const { return (DWORD)(v & 0xFFFFFFFF); }
    static CSteamID Individual(DWORD accountId) { return CSteamID(0x0110000100000000ull | accountId); }
};

enum { kVoiceOK = 0, kVoiceNotRecording = 2, kVoiceNoData = 3, kVoiceBufferTooSmall = 4 };

class ISteamUser016 {
public:
    virtual int GetHSteamUser() = 0;
    virtual bool BLoggedOn() = 0;
    virtual CSteamID GetSteamID() = 0;
    virtual int InitiateGameConnection(void*, int, CSteamID, DWORD, WORD, bool) = 0;
    virtual void TerminateGameConnection(DWORD, WORD) = 0;
    virtual void TrackAppUsageEvent(uint64, int, const char*) = 0;
    virtual bool GetUserDataFolder(char*, int) = 0;
    virtual void StartVoiceRecording() = 0;                                                      // 7
    virtual void StopVoiceRecording() = 0;                                                       // 8
    virtual int GetAvailableVoice(DWORD* cbCompressed, DWORD* cbUncompressed, DWORD rate) = 0;   // 9
    virtual int GetVoice(bool wantCompressed, void* dest, DWORD cbDest, DWORD* written, bool wantUncompressed,
                         void* destRaw, DWORD cbDestRaw, DWORD* writtenRaw, DWORD rate) = 0;     // 10
    virtual int DecompressVoice(const void* data, DWORD cb, void* dest, DWORD cbDest, DWORD* written,
                                DWORD rate) = 0;                                                 // 11
    virtual DWORD GetVoiceOptimalSampleRate() = 0;                                               // 12 (checked live: 24000)
};

enum { kFriendFlagImmediate = 0x04 };
enum { kPersonaOffline = 0 };

#pragma pack(push, 8)
struct FriendGameInfo_t { uint64 m_gameID; DWORD m_unGameIP; WORD m_usGamePort; WORD m_usQueryPort; CSteamID m_steamIDLobby; };
#pragma pack(pop)

class ISteamFriends009 {
public:
    virtual const char* GetPersonaName() = 0;                                                    // 0
    virtual void SetPersonaName(const char*) = 0;
    virtual int GetPersonaState() = 0;
    virtual int GetFriendCount(int flags) = 0;                                                   // 3
    virtual CSteamID GetFriendByIndex(int index, int flags) = 0;                                 // 4
    virtual int GetFriendRelationship(CSteamID) = 0;
    virtual int GetFriendPersonaState(CSteamID id) = 0;                                          // 6
    virtual const char* GetFriendPersonaName(CSteamID id) = 0;                                   // 7
    virtual bool GetFriendGamePlayed(CSteamID id, FriendGameInfo_t* info) = 0;                   // 8
    virtual const char* GetFriendPersonaNameHistory(CSteamID, int) = 0;
    virtual bool HasFriend(CSteamID, int) = 0;
    virtual int GetClanCount() = 0;
    virtual CSteamID GetClanByIndex(int) = 0;
    virtual const char* GetClanName(CSteamID) = 0;
    virtual const char* GetClanTag(CSteamID) = 0;
    virtual int GetFriendCountFromSource(CSteamID) = 0;
    virtual CSteamID GetFriendFromSourceByIndex(CSteamID, int) = 0;
    virtual bool IsUserInSource(CSteamID, CSteamID) = 0;
    virtual void SetInGameVoiceSpeaking(CSteamID, bool) = 0;
    virtual void ActivateGameOverlay(const char* dialog) = 0;                                   // 19
    virtual void ActivateGameOverlayToUser(const char* dialog, CSteamID id) = 0;                // 20
    virtual void ActivateGameOverlayToWebPage(const char*) = 0;
    virtual void ActivateGameOverlayToStore(DWORD) = 0;
    virtual void SetPlayedWith(CSteamID id) = 0;                                                 // 23
    virtual void ActivateGameOverlayInviteDialog(CSteamID lobby) = 0;                            // 24
    virtual int GetSmallFriendAvatar(CSteamID) = 0;
    virtual int GetMediumFriendAvatar(CSteamID) = 0;
    virtual int GetLargeFriendAvatar(CSteamID) = 0;
    virtual bool RequestUserInformation(CSteamID, bool) = 0;
    virtual SteamAPICall_t RequestClanOfficerList(CSteamID) = 0;
    virtual CSteamID GetClanOwner(CSteamID) = 0;
    virtual int GetClanOfficerCount(CSteamID) = 0;
    virtual CSteamID GetClanOfficerByIndex(CSteamID, int) = 0;
    virtual int GetUserRestrictions() = 0;
    virtual bool SetRichPresence(const char* key, const char* value) = 0;                       // 34
    virtual void ClearRichPresence() = 0;                                                        // 35
    virtual const char* GetFriendRichPresence(CSteamID id, const char* key) = 0;                 // 36
};

// Mirrors CCallbackBase of the 2012 SDK; MSVC lays out the overloaded Run()s the same way Valve's build does.
class CCallbackBase {
public:
    CCallbackBase() : m_nCallbackFlags(0), m_iCallback(0) {}
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool ioFailure, SteamAPICall_t call) = 0;
    virtual int GetCallbackSizeBytes() = 0;
    BYTE m_nCallbackFlags;
    int m_iCallback;
};

enum { kLobbyFriendsOnly = 1, kLobbyPublic = 2 };
enum { kLobbyCompareEqual = 0 };
enum { kLobbyDistanceWorldwide = 3 };

class ISteamMatchmaking008 {
public:
    virtual int GetFavoriteGameCount() = 0;
    virtual bool GetFavoriteGame(int, DWORD*, DWORD*, WORD*, WORD*, DWORD*, DWORD*) = 0;
    virtual int AddFavoriteGame(DWORD, DWORD, WORD, WORD, DWORD, DWORD) = 0;
    virtual bool RemoveFavoriteGame(DWORD, DWORD, WORD, WORD, DWORD) = 0;
    virtual SteamAPICall_t RequestLobbyList() = 0;                                              // 4
    virtual void AddRequestLobbyListStringFilter(const char* key, const char* value, int cmp) = 0;  // 5
    virtual void AddRequestLobbyListNumericalFilter(const char* key, int value, int cmp) = 0;
    virtual void AddRequestLobbyListNearValueFilter(const char* key, int value) = 0;
    virtual void AddRequestLobbyListFilterSlotsAvailable(int slots) = 0;                         // 8
    virtual void AddRequestLobbyListDistanceFilter(int filter) = 0;                              // 9
    virtual void AddRequestLobbyListResultCountFilter(int maxResults) = 0;                       // 10
    virtual CSteamID GetLobbyByIndex(int index) = 0;                                             // 11
    virtual SteamAPICall_t CreateLobby(int lobbyType, int maxMembers) = 0;                       // 12
    virtual SteamAPICall_t JoinLobby(CSteamID lobby) = 0;                                        // 13
    virtual void LeaveLobby(CSteamID lobby) = 0;                                                 // 14
    virtual bool InviteUserToLobby(CSteamID lobby, CSteamID invitee) = 0;
    virtual int GetNumLobbyMembers(CSteamID lobby) = 0;                                          // 16
    virtual CSteamID GetLobbyMemberByIndex(CSteamID lobby, int index) = 0;                       // 17
    virtual const char* GetLobbyData(CSteamID lobby, const char* key) = 0;                       // 18
    virtual bool SetLobbyData(CSteamID lobby, const char* key, const char* value) = 0;           // 19
    virtual int GetLobbyDataCount(CSteamID lobby) = 0;
    virtual bool GetLobbyDataByIndex(CSteamID, int, char*, int, char*, int) = 0;
    virtual bool DeleteLobbyData(CSteamID lobby, const char* key) = 0;
    virtual const char* GetLobbyMemberData(CSteamID, CSteamID, const char*) = 0;
    virtual void SetLobbyMemberData(CSteamID, const char*, const char*) = 0;
    virtual bool SendLobbyChatMsg(CSteamID, const void*, int) = 0;
    virtual int GetLobbyChatEntry(CSteamID, int, CSteamID*, void*, int, int*) = 0;
    virtual bool RequestLobbyData(CSteamID lobby) = 0;
    virtual void SetLobbyGameServer(CSteamID, DWORD, WORD, CSteamID) = 0;
    virtual bool GetLobbyGameServer(CSteamID, DWORD*, WORD*, CSteamID*) = 0;
    virtual bool SetLobbyMemberLimit(CSteamID lobby, int maxMembers) = 0;                        // 30
    virtual int GetLobbyMemberLimit(CSteamID lobby) = 0;
    virtual bool SetLobbyType(CSteamID lobby, int lobbyType) = 0;
    virtual bool SetLobbyJoinable(CSteamID lobby, bool joinable) = 0;                            // 33
    virtual CSteamID GetLobbyOwner(CSteamID lobby) = 0;                                          // 34
};

enum { kP2PUnreliable = 0, kP2PReliable = 2 };

class ISteamNetworking005 {
public:
    virtual bool SendP2PPacket(CSteamID remote, const void* data, DWORD size, int sendType, int channel) = 0;
    virtual bool IsP2PPacketAvailable(DWORD* size, int channel) = 0;
    virtual bool ReadP2PPacket(void* dest, DWORD cap, DWORD* size, CSteamID* remote, int channel) = 0;
    virtual bool AcceptP2PSessionWithUser(CSteamID remote) = 0;
    virtual bool CloseP2PSessionWithUser(CSteamID remote) = 0;
    virtual bool CloseP2PChannelWithUser(CSteamID remote, int channel) = 0;
    virtual bool GetP2PSessionState(CSteamID remote, void* state) = 0;
    virtual bool AllowP2PPacketRelay(bool allow) = 0;
};

class ISteamUtils005 {
public:
    virtual DWORD GetSecondsSinceAppActive() = 0;
    virtual DWORD GetSecondsSinceComputerActive() = 0;
    virtual int GetConnectedUniverse() = 0;
    virtual DWORD GetServerRealTime() = 0;
    virtual const char* GetIPCountry() = 0;
    virtual bool GetImageSize(int, DWORD*, DWORD*) = 0;
    virtual bool GetImageRGBA(int, BYTE*, int) = 0;
    virtual bool GetCSERIPPort(DWORD*, WORD*) = 0;
    virtual BYTE GetCurrentBatteryPower() = 0;
    virtual DWORD GetAppID() = 0;                                                                // 9
    virtual void SetOverlayNotificationPosition(int) = 0;
    virtual bool IsAPICallCompleted(SteamAPICall_t call, bool* failed) = 0;                      // 11
    virtual int GetAPICallFailureReason(SteamAPICall_t call) = 0;
    virtual bool GetAPICallResult(SteamAPICall_t call, void* out, int size, int callbackId, bool* failed) = 0;  // 13
};

// Callback payloads (Valve packs them to 8 on Windows).
#pragma pack(push, 8)
struct LobbyCreated_t { enum { k_iCallback = 513 }; int m_eResult; uint64 m_ulSteamIDLobby; };
struct LobbyEnter_t { enum { k_iCallback = 504 }; uint64 m_ulSteamIDLobby; DWORD m_rgfChatPermissions; bool m_bLocked; DWORD m_EChatRoomEnterResponse; };
struct LobbyMatchList_t { enum { k_iCallback = 510 }; DWORD m_nLobbiesMatching; };
struct P2PSessionRequest_t { enum { k_iCallback = 1202 }; CSteamID m_steamIDRemote; };
struct GameLobbyJoinRequested_t { enum { k_iCallback = 333 }; CSteamID m_steamIDLobby; CSteamID m_steamIDFriend; };
struct P2PSessionConnectFail_t { enum { k_iCallback = 1203 }; CSteamID m_steamIDRemote; BYTE m_eP2PSessionError; };
struct LobbyChatUpdate_t {
    enum { k_iCallback = 506 };
    uint64 m_ulSteamIDLobby, m_ulSteamIDUserChanged, m_ulSteamIDMakingChange;
    DWORD m_rgfChatMemberStateChange;  // 1 entered, 2 left, 4 disconnected, 8 kicked, 0x10 banned
};
struct P2PSessionState_t {
    BYTE m_bConnectionActive, m_bConnecting, m_eP2PSessionError, m_bUsingRelay;
    int m_nBytesQueuedForSend, m_nPacketsQueuedForSend;
    DWORD m_nRemoteIP;
    WORD m_nRemotePort;
};
#pragma pack(pop)

struct SteamApi {
    ISteamUser016* user;
    ISteamFriends009* friends;
    ISteamMatchmaking008* mm;
    ISteamNetworking005* net;
    ISteamUtils005* utils;
};

// Null until the game has called SteamAPI_Init; starts the pump thread on first success.
const SteamApi* Steam();

// Completion of an async Steam call, delivered on the pump thread.
typedef void (*AsyncDone)(void* ctx, const void* result, bool failed);
void SteamAsync(SteamAPICall_t call, int callbackId, int resultSize, AsyncDone done, void* ctx);

// Stop the pump before the game shuts Steam down.
void SteamStopPump();

// Register a Steam callback (delivered on the game's thread inside SteamAPI_RunCallbacks).
void SteamRegisterCallback(CCallbackBase* cb, int callbackId);

// Hooks implemented by the transport (net.cpp), sessions (sessions.cpp) and invites (invites.cpp).
void NetPumpReceive(const SteamApi& s);
void SessionsPump(const SteamApi& s);
void InvitesOnSteamReady(const SteamApi& s);  // once, under no lock
void InvitesPump(const SteamApi& s);
