// Nemesis mode (versus, one player controls Nemesis): Xbox 360 only DLC in 2012, but the PC exe has the mode,
// and dlc\pack1 has its levels (NEM_*.swb2), menu pictures and the playable Nemesis. Only the menu gate is shut:
// the game mode table at 0xE9BB88 asks Nemesis (mode 3) for DLC pack 4, and the pack check (0x777160) knows
// packs 1..3 only, so the mode never shows. nemesis=1 in orcsteam.ini clears that requirement.
// Experimental: every player in the game needs it.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "overrides.h"

static const DWORD kRvaModePack = 0xA9BB88;  // DWORD per game mode: DLC pack it needs, 0 = none
static const int kModeNemesis = 3;
static const DWORD kNemesisPack = 4;

void UnlockModes() {
    if (OrcsteamIniInt("nemesis", 0) != 1) return;
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* file = strrchr(exe, '\\');
    if (_stricmp(file ? file + 1 : exe, "RaccoonCity.exe") != 0) return;  // self-test
    DWORD* packs = (DWORD*)((BYTE*)GetModuleHandleA(nullptr) + kRvaModePack);
    // The table as it is in the Steam exe: only Nemesis needs a pack.
    for (int i = 0; i < 7; i++)
        if (packs[i] != (i == kModeNemesis ? kNemesisPack : 0)) {
            OverrideLog("!! nemesis: game mode table does not look as expected, not unlocked");
            return;
        }
    DWORD old;
    if (!VirtualProtect(&packs[kModeNemesis], sizeof(DWORD), PAGE_READWRITE, &old)) {
        OverrideLog("!! nemesis: cannot write the game mode table (%lu)", GetLastError());
        return;
    }
    packs[kModeNemesis] = 0;
    VirtualProtect(&packs[kModeNemesis], sizeof(DWORD), old, &old);
    OverrideLog("nemesis: mode unlocked (experimental, every player needs nemesis=1)");
}
