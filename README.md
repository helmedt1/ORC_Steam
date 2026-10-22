# orcsteam

Online play for **Resident Evil: Operation Raccoon City** (PC, Steam) without Games for Windows Live.

The PC version was built on GFWL, which no longer works. orcsteam is a drop-in `xlive.dll` that implements
every GFWL function the game imports (82 of them) and backs the online ones with Steam:

| GFWL | orcsteam |
|---|---|
| Sign-in, gamertag, XUID | Steam account; persona name transliterated to the Latin-only game font |
| Sessions (`XSession*`), matchmaking search | Steam lobbies carrying the session info and search contexts |
| Game invites, joining friends | Steam overlay invites, "Join game" in the friends list, `+connect_lobby` |
| Secure network (XNet, VDP sockets) | Virtual 10.x.y.z addresses mapped to Steam IDs; datagrams over Steam P2P (relay, no port forwarding) |
| Host migration | Works: the new host republishes the lobby |
| Presence | Steam rich presence: status in "View game info", lobby mates grouped in the friends list |
| Lobby privacy | Public by default; `lobby=friends` in `orcsteam.ini` makes hosted games friends/invite-only |
| Saves (`XLiveProtectData`) | Stored as-is, byte-compatible with the community stub used so far |
| Voice, leaderboards, achievements | Accepted and dropped |

Players see this as: install, start the game from Steam, create or find an online game, or invite Steam friends.
User instructions: [`dist/README.txt`](dist/README.txt) (English), [`dist/ПРОЧТИ.txt`](dist/ПРОЧТИ.txt) (Russian).

**Status (1.1):** two-player co-op campaign tested end to end (search, invites, join from the friends list,
start from an invite, host migration). Not tested yet: 3-4 players, versus modes.

## How it works

- `core.cpp` exports the 82 ordinals `RaccoonCity.exe` imports. Each goes through a two-instruction stub to its
  implementation; with tracing on (`ORCSTEAM_TRACE=1` or `trace=1` in `orcsteam.ini`) every call is logged with
  its arguments and result. A vectored exception handler logs crashes with module offsets and the last call.
- `steam.cpp` talks to the game's own `steam_api.dll` (2012: `SteamMatchMaking008`, `SteamNetworking005`,
  `SteamUtils005`, `SteamFriends009`). Async calls are polled on a pump thread; no newer SDK is needed.
- `sessions.cpp` maps sessions to lobbies, `net.cpp` moves traffic (outgoing datagrams to 10.x.y.z go to
  `SendP2PPacket`; incoming ones are re-injected into the game's socket over loopback with a small header that
  our `recvfrom` strips), `invites.cpp` handles notifications, invites and the friends list, `compat.cpp` the
  rest (sockets, saves, profile defaults, a stub voice engine).

Facts learned from the game binary that the implementation depends on (see `NOTES.md`, in Russian):

- Search results must contain `X_PROPERTY_GAMER_HOSTNAME` (0x40008109); the game copies it without a null check.
- `XINVITE_INFO` and `XONLINE_FRIEND` are 4-byte packed in the game (84 and 0xC4 bytes).
- The community xlive stub popped the wrong number of bytes in `XNetInAddrToString` (#62) and `XInviteSend` (#5316).

## Building

Visual Studio (x86 C++ toolset). From PowerShell:

```
scripts\build.ps1          # build\orcsteam\xlive.dll and build\selftest\selftest.exe
build\selftest\selftest.exe          # offline tests
build\selftest\selftest.exe steam    # + lobbies, search, invites against a running Steam (needs steam_api.dll + steam_appid.txt 209100 next to it)
tools\audit_impl.ps1       # checks every export pops exactly its GFWL argument size
```

`build.ps1` reads the import table of `backup\RaccoonCity.exe` (your own copy of the game executable; it is not
part of this repository) to generate the export list.

## License

MIT, see [`LICENSE`](LICENSE). Not affiliated with Capcom, Microsoft or Valve. You need your own copy of the game.
