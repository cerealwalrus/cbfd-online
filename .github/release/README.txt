CBFD Online
https://github.com/cerealwalrus/cbfd-online

This package contains no game data. You need your own copy of the US ROM of
Conker's Bad Fur Day (N64).

Needs Windows 10 or 11, 64-bit, with a Direct3D 12 or Vulkan graphics card.

Playing
  1. Start ConkerOnline.exe.
  2. The first time, add your ROM with the launcher's Add ROM option: pick your
     US .z64 ROM. It's checked, and remembered for later runs.
  3. Single Player for the one-player game, or Online to host or join a
     multiplayer game (up to 5 players). Everyone needs this same version.

Hosting online needs UDP port 47000 open on your router. The launcher's status
bar says whether it is, and opens it for you over UPnP where the router allows.

Updates: when a newer version is out, the launcher's Update now button
installs it and restarts the game.

ROM hacks that only change the game's assets, such as the uncensored one or the
Russian translation, work too: load one with the launcher's Add ROM option.
Version shows the ROM in play and switches between the ROMs you've loaded.

Save states: F5 saves the game as it is, anywhere, F7 loads it back, and F6
switches between the nine slots. The game's own saves are never touched. More
in docs/save-states.md on the GitHub page.

Texture packs: install a .rtz pack, or a GLideN64 pack's .htc file, with the Mods
menu's Install Mods button (or drop it onto the Mods menu, or put it in the mods
folder), and pick it in Settings > Graphics > Texture Pack. A .htc is unpacked the first
time, which takes a minute or so for a large pack.

Licenses: see LICENSES.txt. The program as a whole is under the GNU GPL version 3
(LICENSE-GPL-3.0.txt), with its source at the GitHub page above.

Saves, settings and the stored ROM are kept in your user folder, not here, so a
newer version can be unpacked anywhere.

If the game crashes, Windows writes crash.log next to ConkerOnline.exe.
