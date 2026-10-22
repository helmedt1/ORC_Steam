#pragma once
// GFWL structures (layouts from the public xlive SDK headers), shared by our implementations.

#include <winsock2.h>  // IN_ADDR only
#include <windows.h>

typedef unsigned long long XUID;

struct XNADDR {
    IN_ADDR ina;
    IN_ADDR inaOnline;
    WORD wPortOnline;
    BYTE abEnet[6];
    BYTE abOnline[20];
};

struct XNKID { BYTE ab[8]; };
struct XNKEY { BYTE ab[16]; };

struct XSESSION_INFO {
    XNKID sessionID;
    XNADDR hostAddress;
    XNKEY keyExchangeKey;
};

struct XOVERLAPPED;
typedef VOID(WINAPI* PXOVERLAPPED_COMPLETION_ROUTINE)(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, XOVERLAPPED* pOverlapped);

struct XOVERLAPPED {
    ULONG_PTR InternalLow;   // status: ERROR_IO_PENDING while running, then the result code
    ULONG_PTR InternalHigh;  // result size / value
    ULONG_PTR InternalContext;
    HANDLE hEvent;
    PXOVERLAPPED_COMPLETION_ROUTINE pCompletionRoutine;
    DWORD_PTR dwCompletionContext;
    DWORD dwExtendedError;
};

struct XUSER_SIGNIN_INFO {
    XUID xuid;
    DWORD dwInfoFlags;
    DWORD UserSigninState;
    DWORD dwGuestNumber;
    DWORD dwSponsorUserIndex;
    CHAR szUserName[16];
};

struct XSESSION_SEARCHRESULT_HEADER {
    DWORD dwSearchResults;
    void* pResults;  // XSESSION_SEARCHRESULT*
};

// XNKID type lives in the top bits of ab[0].
static const BYTE kXnkidOnlinePeer = 0x80;
