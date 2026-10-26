// Stage 3: game traffic over Steam P2P.
// The game sends UDP/VDP datagrams to virtual addresses 10.x.y.z (see FillXnAddrFor). Those are
// wrapped and sent with SendP2PPacket to the owning SteamID. Incoming P2P packets are re-injected
// into the game's own socket over loopback with a small header; our RecvFrom strips it and reports
// the sender's virtual address, so the game's receive thread, select() and blocking all keep working.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xlive_types.h"
#include "overrides.h"
#include "steam.h"

static const WORD kP2PMagic = 0x524F;        // "OR" on the wire, Steam side
static const DWORD kInjectMagic = 0x49435230;  // "0RCI" on loopback
static const int kP2PHeader = 6;               // magic, dst port, src port
static const int kInjectHeader = 10;           // magic, src ip, src port
static const DWORD kUnreliableMax = 1200;      // Steam's limit for unreliable sends
static const int kChannel = 0;
static const int kMaxPeers = 64;
static const DWORD kStatsEveryMs = 60000;
static const DWORD kDiagStatsEveryMs = 10000;
static const LONG kDiagErrors = 20;  // per kind of error, then only counted
static const DWORD kVirtualNet = 0x0A000000, kVirtualMask = 0xFF000000;  // 10.0.0.0/8, host order

struct Peer {
    DWORD ip;
    DWORD accountId;
    LONG sent, recv;
    LONGLONG bytesSent, bytesRecv;
    LONG lastSent, lastRecv;  // at the previous stats line
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static Peer g_peers[kMaxPeers];
static SOCKET g_inject = INVALID_SOCKET;
static WORD g_injectPort;  // network order
static volatile LONG g_dropNoPeer, g_dropSend, g_injectFail, g_recvError, g_recvForeign, g_badMagic;

// diag: how many packets per peer and direction are logged one by one (diag_packets in orcsteam.ini).
static LONG DiagPackets() {
    static LONG n = -1;
    if (n < 0) n = OrcsteamDiag() ? OrcsteamIniInt("diag_packets", 40) : 0;
    return n;
}

static void HexHead(const BYTE* data, int len, char* out) {  // out: at least 3 * 16 + 1
    int n = len < 16 ? len : 16;
    for (int i = 0; i < n; i++) sprintf_s(out + i * 3, 4, "%02X ", data[i]);
    out[n ? n * 3 - 1 : 0] = 0;
}

static void LogPacket(const char* dir, LONG index, DWORD accountId, WORD dstPort, WORD srcPort, const BYTE* data, int len) {
    char hex[3 * 16 + 1];
    HexHead(data, len, hex);
    OverrideLog("net %s #%ld %lu: port %u -> %u, %d bytes: %s", dir, index, accountId, ntohs(srcPort), ntohs(dstPort), len, hex);
}

static bool IsVirtual(DWORD ipNbo) {
    return (_byteswap_ulong(ipNbo) & kVirtualMask) == kVirtualNet;
}

static void RememberPeer(DWORD accountId) {
    DWORD ip = VirtualIpFor(accountId);
    AcquireSRWLockExclusive(&g_lock);
    Peer* freeSlot = nullptr;
    for (Peer& p : g_peers) {
        if (p.ip == ip) {
            if (p.accountId != accountId) OverrideLog("!! virtual address clash: %08lX vs %08lX", p.accountId, accountId);
            p.accountId = accountId;
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        if (!p.ip && !freeSlot) freeSlot = &p;
    }
    if (freeSlot) *freeSlot = {ip, accountId};
    ReleaseSRWLockExclusive(&g_lock);
    if (freeSlot) OverrideLog("peer %lu.%lu.%lu.%lu = steam account %lu", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24, accountId);
}

static Peer* FindPeer(DWORD ip) {  // caller holds g_lock
    for (Peer& p : g_peers)
        if (p.ip == ip) return &p;
    return nullptr;
}

static bool EnsureInjector() {
    if (g_inject != INVALID_SOCKET) return true;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(a);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0 || getsockname(s, (sockaddr*)&a, &len) != 0) {
        closesocket(s);
        return false;
    }
    g_injectPort = a.sin_port;
    g_inject = s;
    OverrideLog("loopback injector on 127.0.0.1:%u", ntohs(a.sin_port));
    return true;
}

// Deliver a datagram to the game's socket bound on dstPort, as if it came from srcIp:srcPort.
static void Inject(WORD dstPort, DWORD srcIp, WORD srcPort, const BYTE* data, int len) {
    if (!EnsureInjector()) return;
    BYTE buf[4096];
    if (len + kInjectHeader > (int)sizeof(buf)) return;
    memcpy(buf, &kInjectMagic, 4);
    memcpy(buf + 4, &srcIp, 4);
    memcpy(buf + 8, &srcPort, 2);
    memcpy(buf + kInjectHeader, data, len);
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = dstPort;
    if (sendto(g_inject, (const char*)buf, len + kInjectHeader, 0, (sockaddr*)&to, sizeof(to)) == SOCKET_ERROR &&
        InterlockedIncrement(&g_injectFail) <= kDiagErrors)
        OverrideLog("!! loopback inject to port %u failed: %d", ntohs(dstPort), WSAGetLastError());
}

static WORD LocalPort(SOCKET s) {
    sockaddr_in a = {};
    int len = sizeof(a);
    return getsockname(s, (sockaddr*)&a, &len) == 0 ? a.sin_port : 0;
}

// Returns false if the destination is not a virtual address (caller forwards to the stub).
static bool SendVirtual(SOCKET s, const sockaddr* to, const BYTE* data, int len) {
    if (!to || to->sa_family != AF_INET) return false;
    const sockaddr_in* dst = (const sockaddr_in*)to;
    if (!IsVirtual(dst->sin_addr.s_addr)) return false;

    WORD srcPort = LocalPort(s);
    if (!srcPort) srcPort = dst->sin_port;
    AcquireSRWLockExclusive(&g_lock);
    Peer* p = FindPeer(dst->sin_addr.s_addr);
    DWORD accountId = p ? p->accountId : 0;
    LONG index = 0;
    if (p) {
        index = ++p->sent;
        p->bytesSent += len;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!accountId) {
        if (InterlockedIncrement(&g_dropNoPeer) <= (OrcsteamDiag() ? kDiagErrors : 5))
            OverrideLog("!! send to unknown virtual address %08lX dropped", dst->sin_addr.s_addr);
        return true;  // UDP: silently lost
    }
    if (index <= DiagPackets()) LogPacket("out", index, accountId, dst->sin_port, srcPort, data, len);
    if (accountId == LocalAccountId()) {
        Inject(dst->sin_port, dst->sin_addr.s_addr, srcPort, data, len);
        return true;
    }

    const SteamApi* steam = Steam();
    BYTE buf[4096];
    if (!steam || len + kP2PHeader > (int)sizeof(buf)) {
        InterlockedIncrement(&g_dropSend);
        return true;
    }
    memcpy(buf, &kP2PMagic, 2);
    memcpy(buf + 2, &dst->sin_port, 2);
    memcpy(buf + 4, &srcPort, 2);
    memcpy(buf + kP2PHeader, data, len);
    DWORD total = len + kP2PHeader;
    if (!steam->net->SendP2PPacket(CSteamID::Individual(accountId), buf, total,
                                   total <= kUnreliableMax ? kP2PUnreliable : kP2PReliable, kChannel) &&
        InterlockedIncrement(&g_dropSend) <= (OrcsteamDiag() ? kDiagErrors : 0))
        OverrideLog("!! SendP2PPacket to %lu failed (%lu bytes)", accountId, total);
    return true;
}

static void LogStats(const SteamApi& s) {
    struct Line { DWORD accountId; LONG sent, recv, dSent, dRecv; LONGLONG bytesSent, bytesRecv; };
    Line lines[kMaxPeers];
    int n = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (Peer& p : g_peers) {
        if (!p.ip || !(p.sent || p.recv)) continue;
        lines[n++] = {p.accountId, p.sent, p.recv, p.sent - p.lastSent, p.recv - p.lastRecv, p.bytesSent, p.bytesRecv};
        p.lastSent = p.sent;
        p.lastRecv = p.recv;
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (int i = 0; i < n; i++) {
        const Line& l = lines[i];
        if (!OrcsteamDiag()) {
            OverrideLog("net: account %lu sent %ld, recv %ld", l.accountId, l.sent, l.recv);
            continue;
        }
        // diag: also what Steam itself thinks of the connection
        P2PSessionState_t st = {};
        bool known = l.accountId != LocalAccountId() && s.net->GetP2PSessionState(CSteamID::Individual(l.accountId), &st);
        OverrideLog("net: account %lu sent %ld (+%ld, %lld B), recv %ld (+%ld, %lld B); steam %s%s%s, error %u, queued %d pkt / %d B",
                    l.accountId, l.sent, l.dSent, l.bytesSent, l.recv, l.dRecv, l.bytesRecv,
                    !known ? "no session" : st.m_bConnectionActive ? "active" : "inactive",
                    known && st.m_bConnecting ? ", connecting" : "", known && st.m_bUsingRelay ? ", relay" : "",
                    st.m_eP2PSessionError, st.m_nPacketsQueuedForSend, st.m_nBytesQueuedForSend);
    }
    if (g_dropNoPeer || g_dropSend || g_injectFail || g_recvError || g_recvForeign || g_badMagic)
        OverrideLog("net: dropped %ld (no peer), %ld (steam send failed), %ld (inject failed); recv errors %ld, "
                    "foreign datagrams %ld, bad p2p magic %ld",
                    g_dropNoPeer, g_dropSend, g_injectFail, g_recvError, g_recvForeign, g_badMagic);
}

void NetPumpReceive(const SteamApi& s) {
    static DWORD lastStats = GetTickCount();
    DWORD size;
    while (s.net->IsP2PPacketAvailable(&size, kChannel)) {
        BYTE buf[4096];
        CSteamID from;
        if (!s.net->ReadP2PPacket(buf, sizeof(buf), &size, &from, kChannel)) break;
        WORD magic;
        memcpy(&magic, buf, 2);
        if (size < (DWORD)kP2PHeader || magic != kP2PMagic) {
            if (InterlockedIncrement(&g_badMagic) <= (OrcsteamDiag() ? kDiagErrors : 0))
                OverrideLog("!! p2p packet from %lu without our header (%lu bytes)", from.AccountId(), size);
            continue;
        }
        DWORD accountId = from.AccountId();
        RememberPeer(accountId);
        DWORD ip = VirtualIpFor(accountId);
        LONG index = 0;
        AcquireSRWLockExclusive(&g_lock);
        if (Peer* p = FindPeer(ip)) {
            index = ++p->recv;
            p->bytesRecv += size - kP2PHeader;
        }
        ReleaseSRWLockExclusive(&g_lock);
        WORD dstPort, srcPort;
        memcpy(&dstPort, buf + 2, 2);
        memcpy(&srcPort, buf + 4, 2);
        if (index && index <= DiagPackets())
            LogPacket("in", index, accountId, dstPort, srcPort, buf + kP2PHeader, (int)size - kP2PHeader);
        Inject(dstPort, ip, srcPort, buf + kP2PHeader, size - kP2PHeader);
    }
    if (GetTickCount() - lastStats >= (OrcsteamDiag() ? kDiagStatsEveryMs : kStatsEveryMs)) {
        LogStats(s);
        lastStats = GetTickCount();
    }
}

void NetAcceptPeer(DWORD accountId) {
    RememberPeer(accountId);
    if (const SteamApi* s = Steam())
        if (accountId != LocalAccountId()) s->net->AcceptP2PSessionWithUser(CSteamID::Individual(accountId));
}

// ---- socket overrides ----

static int WINAPI My_XSocketRecvFrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen) {
    sockaddr_in src = {};
    int srcLen = sizeof(src);
    int n = recvfrom(s, buf, len, flags, (sockaddr*)&src, &srcLen);
    if (n == SOCKET_ERROR && OrcsteamDiag()) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && InterlockedIncrement(&g_recvError) <= kDiagErrors)
            OverrideLog("!! recvfrom on socket %u (port %u) failed: %d", (unsigned)s, ntohs(LocalPort(s)), err);
        WSASetLastError(err);  // the game reads it
    }
    DWORD magic = 0;
    if (n >= kInjectHeader) memcpy(&magic, buf, 4);
    if (n >= kInjectHeader && magic == kInjectMagic && src.sin_family == AF_INET &&
        src.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && src.sin_port == g_injectPort) {
        memcpy(&src.sin_addr.s_addr, buf + 4, 4);
        memcpy(&src.sin_port, buf + 8, 2);
        n -= kInjectHeader;
        memmove(buf, buf + kInjectHeader, n);
    } else if (n > 3 && OrcsteamDiag() && InterlockedIncrement(&g_recvForeign) <= kDiagErrors) {
        // not from Steam (the game's own 3-byte wake-ups are skipped)
        OverrideLog("net: socket %u got %d bytes from %u.%u.%u.%u:%u, not via steam", (unsigned)s, n,
                    src.sin_addr.S_un.S_un_b.s_b1, src.sin_addr.S_un.S_un_b.s_b2, src.sin_addr.S_un.S_un_b.s_b3,
                    src.sin_addr.S_un.S_un_b.s_b4, ntohs(src.sin_port));
    }
    if (n >= 0 && from && fromlen) {
        int copy = *fromlen < srcLen ? *fromlen : srcLen;
        memcpy(from, &src, copy);
        *fromlen = srcLen;
    }
    return n;
}

static int WINAPI My_XSocketSendTo(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
    if (SendVirtual(s, to, (const BYTE*)buf, len)) return len;
    return sendto(s, buf, len, flags, to, tolen);
}

static int WINAPI My_XWSASendTo(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags, const sockaddr* to,
                                int tolen, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE routine) {
    if (to && to->sa_family == AF_INET && IsVirtual(((const sockaddr_in*)to)->sin_addr.s_addr)) {
        BYTE joined[4096];
        int total = 0;
        for (DWORD i = 0; i < count && total + (int)bufs[i].len <= (int)sizeof(joined); i++) {
            memcpy(joined + total, bufs[i].buf, bufs[i].len);
            total += bufs[i].len;
        }
        SendVirtual(s, to, joined, total);
        if (sent) *sent = total;
        if (ov && ov->hEvent) SetEvent(ov->hEvent);
        return 0;
    }
    return WSASendTo(s, bufs, count, sent, flags, to, tolen, ov, routine);
}

// ---- XNet: addresses and keys ----

static INT WINAPI My_XNetXnAddrToInAddr(const XNADDR* xna, const XNKID* kid, IN_ADDR* ina) {
    if (!xna || !ina) return WSAEINVAL;
    if (!IsVirtual(xna->ina.S_un.S_addr)) {  // not a Steam player: no route (the old stub answered 0.0.0.0)
        ina->s_addr = 0;
        return 0;
    }
    NetAcceptPeer(XnAddrAccountId(xna));
    *ina = xna->ina;
    static volatile LONG logged;
    if (OrcsteamDiag() && InterlockedIncrement(&logged) <= 60)
        OverrideLog("xnet: xnaddr of account %lu -> %u.%u.%u.%u", XnAddrAccountId(xna), ina->S_un.S_un_b.s_b1,
                    ina->S_un.S_un_b.s_b2, ina->S_un.S_un_b.s_b3, ina->S_un.S_un_b.s_b4);
    return 0;
}

static INT WINAPI My_XNetInAddrToXnAddr(IN_ADDR ina, XNADDR* xna, XNKID* kid) {
    AcquireSRWLockShared(&g_lock);
    Peer* p = FindPeer(ina.S_un.S_addr);
    DWORD accountId = p ? p->accountId : 0;
    ReleaseSRWLockShared(&g_lock);
    if (!accountId) {  // unknown address: outputs untouched, like the old stub
        static volatile LONG logged;
        if (OrcsteamDiag() && InterlockedIncrement(&logged) <= kDiagErrors)
            OverrideLog("!! xnet: no xnaddr for unknown address %u.%u.%u.%u", ina.S_un.S_un_b.s_b1, ina.S_un.S_un_b.s_b2,
                        ina.S_un.S_un_b.s_b3, ina.S_un.S_un_b.s_b4);
        return 0;
    }
    if (xna) FillXnAddrFor(accountId, xna);
    if (kid) ActiveSessionKid(kid);
    return 0;
}

static INT WINAPI My_XNetRegisterKey(const XNKID* kid, const XNKEY* key) {
    return 0;  // Steam P2P handles encryption; nothing to register
}

// The stub's #62 pops no arguments (stdcall needs 12 bytes) and crashes the caller; see tools/audit_stub.ps1.
static INT WINAPI My_XNetInAddrToString(IN_ADDR ina, char* buf, INT cch) {
    if (!buf || cch <= 0) return WSAEINVAL;
    sprintf_s(buf, cch, "%u.%u.%u.%u", ina.S_un.S_un_b.s_b1, ina.S_un.S_un_b.s_b2, ina.S_un.S_un_b.s_b3,
              ina.S_un.S_un_b.s_b4);
    return 0;
}

// ---- QoS: every virtual host answers instantly with a good link ----

struct XNQOSINFO {
    BYTE bFlags;
    BYTE bReserved;
    WORD cProbesXmit;
    WORD cProbesRecv;
    WORD cbData;
    BYTE* pbData;
    WORD wRttMinInMsecs;
    WORD wRttMedInMsecs;
    DWORD dwUpBitsPerSec;
    DWORD dwDnBitsPerSec;
};
struct XNQOS {
    UINT cxnqos;
    UINT cxnqosPending;
    XNQOSINFO axnqosinfo[1];
};
static const BYTE kQosComplete = 0x01, kQosTargetContacted = 0x02;
static const WORD kQosRttMs = 50;
static const DWORD kQosBitsPerSec = 4000000;
static XNQOS* g_qosAllocs[16];

static INT WINAPI My_XNetQosLookup(UINT cxna, const XNADDR* apxna[], const XNKID* apxnkid[], const XNKEY* apxnkey[],
                                   UINT cina, const IN_ADDR aina[], const DWORD adwServiceId[], UINT cProbes,
                                   DWORD dwBitsPerSec, DWORD dwFlags, HANDLE hEvent, XNQOS** ppxnqos) {
    if (!ppxnqos) return WSAEINVAL;
    UINT total = cxna + cina;
    size_t size = sizeof(XNQOS) + (total ? total - 1 : 0) * sizeof(XNQOSINFO);
    XNQOS* q = (XNQOS*)calloc(1, size);
    if (!q) return WSAENOBUFS;
    q->cxnqos = total;
    q->cxnqosPending = 0;
    for (UINT i = 0; i < total; i++) {
        XNQOSINFO& info = q->axnqosinfo[i];
        info.bFlags = kQosComplete | kQosTargetContacted;
        info.cProbesXmit = info.cProbesRecv = (WORD)(cProbes ? cProbes : 1);
        info.wRttMinInMsecs = info.wRttMedInMsecs = kQosRttMs;
        info.dwUpBitsPerSec = info.dwDnBitsPerSec = kQosBitsPerSec;
        if (i < cxna && apxna && apxna[i]) NetAcceptPeer(XnAddrAccountId(apxna[i]));
    }
    AcquireSRWLockExclusive(&g_lock);
    for (XNQOS*& slot : g_qosAllocs)
        if (!slot) { slot = q; break; }
    ReleaseSRWLockExclusive(&g_lock);
    *ppxnqos = q;
    if (hEvent) SetEvent(hEvent);
    OverrideLog("qos lookup: %u hosts, %u probes -> all reachable", total, cProbes);
    return 0;
}

static INT WINAPI My_XNetQosRelease(XNQOS* q) {
    bool ours = false;
    AcquireSRWLockExclusive(&g_lock);
    for (XNQOS*& slot : g_qosAllocs)
        if (q && slot == q) { slot = nullptr; ours = true; }
    ReleaseSRWLockExclusive(&g_lock);
    if (ours) free(q);
    return 0;
}

const Override kNetOverrides[] = {
    {20, (void*)My_XSocketRecvFrom},
    {24, (void*)My_XSocketSendTo},
    {25, (void*)My_XWSASendTo},
    {55, (void*)My_XNetRegisterKey},
    {57, (void*)My_XNetXnAddrToInAddr},
    {60, (void*)My_XNetInAddrToXnAddr},
    {62, (void*)My_XNetInAddrToString},
    {70, (void*)My_XNetQosLookup},
    {72, (void*)My_XNetQosRelease},
};
const int kNetOverrideCount = sizeof(kNetOverrides) / sizeof(kNetOverrides[0]);
