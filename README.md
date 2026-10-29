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
| Host migration | Works: peers adopt the new host's session info; whoever Steam hands the lobby to republishes it |
| Presence | Steam rich presence player group: lobby mates are grouped in the friends list |
| Lobby privacy | Public by default; `lobby=friends` in `orcsteam.ini` makes hosted games friends/invite-only |
| Saves (`XLiveProtectData`) | Stored as-is, byte-compatible with the community stub used so far |
| Voice (`XHV`) | A real engine behind the game's own voice chat: Steam voice (`GetVoice`/`DecompressVoice`) carried in the game's voice messages, played with XAudio2; `voice`, `voice_key`, `voice_volume` in `orcsteam.ini` |
| Spatial voice | Each voice message also carries the talker's character position and acoustic environment, read from the game's Wwise (the exe exports it); the listener applies distance volume, panning, XAudio2 reverb per environment and a band-passed "radio" beyond `voice_far` |
| Leaderboards, achievements | Accepted and dropped |

Extras, off by default (`orcsteam.ini`): `nemesis=1` shows the Nemesis versus mode (Xbox 360 only DLC in 2012;
the PC exe and `dlc\pack1` have the mode and its maps, only the menu's DLC pack check hides it; experimental),
`skip_intro=1` skips the startup logos and movie, `fov=<percent>` scales every camera's field of view.

Players see this as: install, start the game from Steam, create or find an online game, or invite Steam friends.
User instructions: [`dist/README.txt`](dist/README.txt) (English), [`dist/ПРОЧТИ.txt`](dist/ПРОЧТИ.txt) (Russian).

**Status (1.5):** co-op campaign with two and four players tested end to end (search, invites, join from the
friends list, start from an invite, host migration). Voice chat (new in 1.3) and spatial voice (1.4) are
beta: tested with two and four players. Nemesis mode: a short game with four players. Not tested yet: the
other versus modes. Known issue: after a host change, a player who left cannot rejoin the same mission.

## How it works

- `core.cpp` exports the 82 ordinals `RaccoonCity.exe` imports. Each goes through a two-instruction stub to its
  implementation; with tracing on (`ORCSTEAM_TRACE=1` or `trace=1` in `orcsteam.ini`) every call is logged with
  its arguments and result; `diag=1` adds a network log (per-peer packets, Steam P2P session state, lobby
  members, sockets). A vectored exception handler logs crashes with module offsets and the last call.
- `steam.cpp` talks to the game's own `steam_api.dll` (2012: `SteamMatchMaking008`, `SteamNetworking005`,
  `SteamUtils005`, `SteamFriends009`). Async calls are polled on a pump thread; no newer SDK is needed.
- `sessions.cpp` maps sessions to lobbies, `net.cpp` moves traffic (outgoing datagrams to 10.x.y.z go to
  `SendP2PPacket`; incoming ones are re-injected into the game's socket over loopback with a small header that
  our `recvfrom` strips), `invites.cpp` handles notifications, invites and the friends list, `voice.cpp` the
  voice engine, `spatial.cpp` the Wwise hooks that tell where characters and the camera are and which acoustic
  environment they are in, `unlock.cpp` the Nemesis menu gate, `tweaks.cpp` skip_intro and fov,
  `compat.cpp` the rest (sockets, saves, profile defaults).

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

Includes [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4 (BSD 2-clause, [`third_party/minhook/LICENSE.txt`](third_party/minhook/LICENSE.txt)),
for the Wwise hooks in `spatial.cpp`.
