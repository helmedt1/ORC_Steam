orcsteam 1.0 - online play for Resident Evil: Operation Raccoon City over Steam
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


TESTED
Co-op campaign with 2 players: search, invites, "Join game", start from an invite,
host change. Not tested yet: 3-4 players, versus modes.

NOT SUPPORTED
Voice chat, leaderboards, achievements (the old service is gone; nothing is sent).


SAVES
Your saves are untouched: orcsteam reads and writes them exactly like before.


IF SOMETHING GOES WRONG
- orcsteam.log in the game folder shows what happened. Please attach it to any report.
- For a detailed log, create orcsteam.ini next to xlive.dll with:
      [orcsteam]
      trace=1
  (the log gets large; remove the file afterwards).
- Some antivirus programs distrust unknown DLLs. orcsteam only talks to Steam;
  the source code is published with the mod.
