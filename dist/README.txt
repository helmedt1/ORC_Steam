orcsteam 1.5 - online play for Resident Evil: Operation Raccoon City over Steam
==============================================================================

The PC version was built on Games for Windows Live, which is gone. orcsteam is a
replacement xlive.dll that gives the game back its online modes through Steam:
sign-in uses your Steam account, games are Steam lobbies, and game traffic goes
over Steam's peer-to-peer network (no port forwarding needed).

Every player needs a legitimate copy of the game on Steam and orcsteam installed.


INSTALL
1. Steam -> Library -> Resident Evil: Operation Raccoon City -> right-click ->
   Manage -> Browse local files. This opens the folder with RaccoonCity.exe.
2. Copy install.bat, uninstall.bat and the orcsteam folder into it.
3. Run install.bat.

To remove orcsteam, run uninstall.bat. "Verify integrity of game files" in Steam
also puts the original files back.


PLAYING
- Start the game from Steam (Steam must be running). Your Steam name is your
  in-game name (non-Latin letters are transliterated: the game font is Latin-only).
- Find a game: create or search for an online game in the game's menus as usual.
- Play with Steam friends: the host creates an online game and waits in the lobby,
  then invites friends with Shift+Tab -> friends list -> right-click -> Invite to game.
  Friends accept the invite, or right-click the host in their friends list -> Join game.
  If a friend's game is not running, Steam starts it and joins about 20 s after launch.
- If the host leaves, another player becomes the host and the game goes on.
- Players of the same game are grouped together in the Steam friends list.


VOICE CHAT
The game's own voice chat works: talking icons, muting a player and headset volume
are in the game's menus as originally designed. Steam records and compresses the
voice, so the microphone is chosen in Steam: Settings -> Voice (with a microphone test
and the activation threshold). By default the microphone is open and voice-activated.
Only players with orcsteam 1.3 or newer hear each other.

Spatial voice (1.4): a teammate sounds from where their character stands - loud nearby,
quieter far away, from the left or right, with the echo of a room, tunnel or sewer.
Beyond 20 metres their voice turns into a "radio": a steady level and a radio-like sound,
so you stay in touch anywhere on the map. Only players with orcsteam 1.4 sound spatial;
players with 1.3 sound as before, without distance.


NEMESIS MODE (experimental)
In 2012 the Nemesis versus mode was an Xbox 360 DLC, but the PC game already has the
mode, its six maps and the playable Nemesis on disk - only the menu hides it.
nemesis=1 in orcsteam.ini (see SETTINGS) shows it in the versus menu. Every player in
the game needs nemesis=1. Played with 4 players for a few minutes; longer games and
all six maps are not tested yet - reports welcome.


SETTINGS (optional)
Create orcsteam.ini next to xlive.dll in the game folder:
    [orcsteam]
    lobby=friends
- lobby=friends: games you host are visible only to your Steam friends and people
  you invite. They do not show up in anyone's in-game search, so friends join with
  an invite or "Join game". Without this line (default) your games are public.
- trace=1: a detailed log for bug reports (the log gets large; remove it afterwards).
- diag=1: a detailed network log (packets, Steam connection state) for connection problems.
- voice=0: turn voice chat off (you hear no one, and no one hears you).
- voice_key=V: talk only while the key is held (push-to-talk). A letter or digit, F1-F12,
  mouse3, mouse4, mouse5, capslock, alt, ctrl, shift, tab or space. Without it: open mic.
- voice_volume=150: volume of all other players in percent (0-400, default 100).
  Quiet voices are already levelled automatically; this comes on top.
- voice_spatial=0: no spatial voice - everyone sounds the same, as in 1.3.
- voice_near=5: up to how many metres a voice is at full volume (default 5).
- voice_far=20: from which distance the radio takes over (default 20).
- voice_radio=0: no radio - far teammates fade out completely.
- voice_reverb=0: no echo.
- skip_intro=1: skip the logos and the NVIDIA movie at startup, straight to the press-start screen.
- fov=120: camera field of view in percent of the game's own (50-200, default 100). 120 = 20% wider.
- nemesis=1: the Nemesis versus mode (experimental, see NEMESIS MODE).


TESTED
Co-op campaign with 2 and 4 players: search, invites, "Join game", start from an
invite, host change. Nemesis mode with 4 players (a short game). Not tested yet: the
other versus modes.
Voice chat and spatial voice are beta: tested with 2 and 4 players. If something sounds
wrong, please send orcsteam.log (preferably with diag=1 in orcsteam.ini) and describe it.

Known issue: after a host change, a player who left cannot rejoin the same mission
through "Join game"; they get in once the new host starts the next mission.


WHAT'S NEW IN 1.5
- Nemesis mode (experimental): nemesis=1 shows the Xbox-only versus mode, see NEMESIS MODE.
- skip_intro=1: no logos and NVIDIA movie at startup.
- fov=<percent>: a wider (or narrower) camera field of view.
- Voices are louder: quiet voices are levelled higher, so music drowns them out less.
- The radio starts at 20 m instead of 40 (the maps are small) and is louder.
- Fixed a crash with diag=1 when a character stood in several sound zones with long names.

WHAT'S NEW IN 1.4
- Spatial voice: volume by distance, direction, echo for where the talker stands, and a radio
  for far teammates (see VOICE CHAT). Settings voice_spatial, voice_near, voice_far,
  voice_radio, voice_reverb.
- Voice no longer loses pieces of speech (the 1.3 test builds dropped parts of phrases, 1.4-test1
  the start of some phrases).
- Quiet voices are levelled automatically: Steam's voice is much quieter than the game.

WHAT'S NEW IN 1.3
- Voice chat: the game's built-in voice chat works (see VOICE CHAT), settings voice,
  voice_key, voice_volume.

WHAT'S NEW IN 1.2
- Host change with 4 players: players who do not become the host no longer take
  the host role themselves, and the Steam lobby points to the new host afterwards
  ("Join game" after the host left used to lead into a closed game).
- The fourth player no longer drops out of the Steam lobby 2 minutes after joining.
- diag=1 setting for a detailed network log.

NOT SUPPORTED
Leaderboards, achievements (the old service is gone; nothing is sent).


SAVES
Your saves are untouched: orcsteam reads and writes them exactly like before.


IF SOMETHING GOES WRONG
- orcsteam.log in the game folder shows what happened. Please attach it to any report.
- For a detailed log, add trace=1 to orcsteam.ini (see SETTINGS).
- Some antivirus programs distrust unknown DLLs. orcsteam only talks to Steam;
  the source code is published with the mod.
