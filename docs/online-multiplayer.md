# Online multiplayer

The game's local multiplayer (the arena modes, 2 to 4 players) played over a network, each player with a full-screen view
instead of a split screen. Everything is deterministic lockstep: the machines exchange only controller inputs, each runs
the whole game, and the host sends a snapshot to put a machine right if its random numbers ever drift apart.

A game is set up in the launcher's **Online** screen (the lobby), or with environment variables (below, for testing).

## The lobby

Both players need the same build, and the ROM loaded (Start Game's Load ROM does that once).

1. The host chooses **Online** in the launcher, sets Players (2 to 4), Input delay and Controller, and presses **Host**. The
   lobby lists the players as they join. The host's router has to forward the port (TCP, 47000 by default) to the host's
   computer, and the others need the host's public IP address.
2. Each other player chooses **Online**, types the host's address (`address` or `address:port`), and presses **Join**.
3. When everybody is in, the host presses **Start game**; every machine starts the game, and the host and the players choose
   the multiplayer mode in the game's own menu as usual. Anybody can **Leave** before that (and join again); a version
   mismatch, a full game, a wrong address or a closed port is explained on the screen.

The session's log is written next to the program (`netplay_host.txt` or `netplay_join.txt`; `CONKER_NET_LOG` picks another
file). The lobby is for one online game per run of the program; close it and start it again for another. The lobby doesn't
turn on `CONKER_IGNORE_DIRECTINPUT` by itself (it has to be set before the program starts): set it if a gamepad shows up twice.

## Playing

The host is player 1 (slot 0), joiners are players 2 to 4. Every machine needs its own copy of the ROM and a build.

Host (a TCP port, forwarded on the router for play over the internet):

    set CONKER_NET_HOST=47000
    set CONKER_NET_PLAYERS=2
    ConkerRecomp.exe

Joiner:

    set CONKER_NET_JOIN=host-address:47000
    ConkerRecomp.exe

Choose the multiplayer mode in the menu on every machine, host first. The game starts when all the players are in.

## Chat

During an online game, **Y on the keyboard** (or the key chosen in Settings, Online tab, Chat: Key; it can be turned off) opens a line to type on (bottom-left of the screen). **Enter** sends it; Enter on an
empty line closes the box (Esc isn't used: it opens the settings menu). Everybody sees the lines for ten seconds, coloured by
player. While you type your own controller is held still, and for half a second after, so the Enter isn't a press of Start. The
lines go through the host, and are limited to 120 characters and five lines in four seconds per player. There's no gamepad
typing: a gamepad's Y does nothing. The key does what it's bound to in Controls as well, so pick one that isn't used. The chat is in the session's log too (`[chat]` lines).

## Settings

| Variable | Meaning |
| --- | --- |
| `CONKER_NET_HOST=<port>` | host a game on that TCP port |
| `CONKER_NET_PLAYERS=<2..4>` | how many players the host waits for (default 2) |
| `CONKER_NET_JOIN=<host>:<port>` | join a game |
| `CONKER_NET_DELAY=<frames>` | input delay (default 3, about 100 ms; raise it for a slower connection) |
| `CONKER_NET_INPUT=keyboard\|gamepad\|gamepad2` | this machine's player uses only that (for two windows on one PC) |
| `CONKER_NET_LOG=<file>` | a log of the session |
| `CONKER_IGNORE_DIRECTINPUT=1` | ignore controllers that are not XInput ones (on by default online; `CONKER_ALLOW_DIRECTINPUT=1` turns it off) |
| `CONKER_NET_VIEW=split` | keep the split screen (to compare) |
| `CONKER_NET_ALLOWPAUSE=1` | let the Start button pause the game online (it stops it for everyone; off by default, the game goes on) |
| `CONKER_NET_KEEPCULL=1` | use the game's own choice of which players are drawn for a camera |

Debugging aids: `CONKER_NET_TEST_DESYNC`, `CONKER_NET_NORESYNC`, `CONKER_NET_VIEWMASK`, `CONKER_NET_AIMFIT`,
`CONKER_NET_HUD`, `CONKER_NET_SCORESWAP`, `CONKER_NET_HIDEOLD`, `CONKER_NET_VIEWSLOT`, `CONKER_NET_GFXSYNC`,
`CONKER_NET_KEEPREWRITE` and the `CONKER_NETPLAY_*` recording and replay ones.

## Logs

Hosting or joining from the launcher always writes the session's log: `netplay_host.txt` or `netplay_join.txt` in the folder the game was started from
(or in the settings folder, if that one can't be written to). The lobby shows the full path. `CONKER_NET_LOG=<file>` picks another file.

## Aiming a gun in R-Look online

The SMG and the M4 aim in the game's R-Look (kinds 0x39 and 0x09, `+0x23E` of the camera). At full stick the view turned left and right at 4.06
degrees a frame but up and down at 1.9, and up and down also eased by the spring (about 14 frames to settle), which felt slow and drunk. Online,
for these two kinds, the stick's pitch is scaled to the yaw's rate and the view's pitch takes the target at once (`gun_aim_online` in
`look_aim.cpp`). It isn't a setting: the game's state has to be the same on every machine. The sniper's scope is another aiming mode and is untouched.

## The aiming reticle online

Aiming: Reticle (Camera tab) works online too: it is drawn on this machine's own full-screen view (`reticle.cpp`), for the slingshot, the knives
and the magnum as offline, and for the SMG and the M4 (the look mode's states 0x39 and 0x09) in the middle of the view. It is a local overlay and
does not touch the game's state, so each player can turn it on or off for themselves.

## Nicknames

The launcher's Online screen has a Nickname box (kept in `online_nickname.txt` beside the settings, up to 16 characters). It is sent when joining and
the host tells everybody the names in the lobby info, so the lobby, the chat and the "left the match" notices use them; empty, a player is
"Player N". Two players with the same name: the later one gets " 2". Names are only for showing, not part of the game. Protocol version 5.

## The right-stick camera online

With Right Stick: Free Camera on (Camera tab), the right stick turns each player's own camera online too. The turn has to be the same on every
machine, because the game moves a player by the direction their camera faces. So each machine sends, with its input for a frame, how far the
stick turns and tilts its player's camera (already scaled by that player's Camera: Turning Speed and Invert Turning, in 1/10000 of a radian;
`PlayerInput::cam_*`), and every machine runs the orbit camera (`mouse_camera.cpp`) for every player's camera from those numbers, at a fixed
1/30 s a frame instead of the clock, with trigonometry made of plain arithmetic (the C library's is picked by the processor).
A snapshot (a resync) starts every orbit over on all machines at the same moment, since the orbit's own memory isn't in it.
The mouse still doesn't turn the camera online. With the setting off the stick is the C-buttons as before. Protocol version 4.

## How the full-screen view works

`host/src/net_view.cpp` rewrites each frame's finished display list before it is drawn (the camera's viewport and scissor
become the whole screen, its projection is widened to match, the HUD is laid out again) and puts the game's words back
afterwards, so the game's memory is the same on every machine. The players are drawn for every camera, whatever the
distance (`conker_player_visibility`): the game hid the caveman from the raptor's camera far away.

## The game's sounds

The game decides some things (which sound slot to use, when a voice line is over, timers that follow from that) from whether a sound
is still playing, and the audio thread of each machine plays at its own pace, so the machines used to drift apart and the host had to
resync them. Now the host looks at its real audio at the start of each frame and sends, with its input, which sounds have finished
(`PlayerInput::ended`); every machine acts on that at the same frame, the audio thread no longer touches the game's sound slots, and
the game's "is it still playing?" answers from the slot alone (`conker_net_sound_hook`, hooks in `conker.toml`, notes in
`host/src/net_session.cpp`). In a 20-minute test over the internet this took the resyncs from about one every 5 seconds to none.
Every machine has to run the same build.

## Known limits

- During a match the Start button does not pause (the game's pause stops everything, and every machine has to go on together). It opens the game's own multiplayer pause screen instead (the PAUSED logo, the stats and the team scores), on that player's screen only: the match goes on behind it, that player's character stands still, and the normal HUD (health, radars, weapon bar) is hidden while it is open. There are two buttons, CONTINUE and QUIT; RESTART is not offered, since it would restart one machine's game alone. Quit (confirmed with Yes) ends the match for everybody: the quitter's input carries the values the game's own quit sets (D_800E0B94 and a few more, see `net_session.cpp`), every machine sets them at the same frame, and all players go back to the game's multiplayer menu together, still connected, with a "Player N ended the match" notice. The menu needs a free player slot, so it is not available in a four-player match yet.
- A machine that cannot be reached for a while is dropped: the others see "Player N left the match" and go on, and a joiner whose host is gone sees a notice and goes on offline.
- Audio is the local listener's own, positional sound is not yet per player.
- Tested with 2 players; 3 and 4 are untested.

## Aiming: Swap Sticks online (gamepad)

With **Aiming: Swap Sticks** ON and a gamepad, aiming in R-Look online (SMG, M4, ...) makes the right stick aim and the left stick
move Conker, as in single player. The swap is sent with each frame's input, so every machine applies it on the same frame. Keyboard
and mouse are unchanged. While the sticks are swapped the right stick does not drive the free camera.

The view's yaw also follows the aim at once from the first frame of an R-Look gun aim online (it used to spring in from where the camera
was, while the head followed the stick, so holding the stick as R was pressed turned the head ahead of the view).

Holding R, the game's look mode has stages (+0x73C of the camera: 0 not started, 1 moving in for 30 frames, 2 aiming, 3 leaving). In
stage 1 the camera's yaw eased toward a target worked out once, while Conker turned with the stick, so the head turned and the view
lagged then caught up. Online, at the start of func_15120158 in stage 1 the target and the current yaw are set to the aim's each frame
(conker_look_stage_one), so the view is on the aim from the first frame and the stick aims at once. While the stage is 0 with R held
(the game hasn't started the look mode yet) the local gamepad's right stick does nothing (frontend.cpp, net_session.cpp).

With no gun (kind 0, the plain R-look) the stick turns the view's own target, which only the look mode's second stage reads, so the
first stage's 30 frames did nothing. Online the look mode goes straight on to the second stage (conker_look_stage_one), whose springs
move the camera in, so the stick aims at once.

Aiming: Swap Sticks, no weapon: in the plain R-look Conker stands still, and of the C-buttons only C-Left and C-Right (strafing) move him.
With Swap Sticks ON the left stick's direction from the view (as with a gun) becomes C-Left alone with his facing set so the strafe goes
that way (look_aim.cpp, no_weapon_walk, called for the game's read of the controller), and his facing is put back to the view's afterwards.

No weapon walking, drawn: the game gets C-Left (C-Right when pressed right) with his facing on the view, so he is drawn and animated as its own
strafe whichever way the left stick points; conker_look_targets then sets his moving angle (+0x76) to the way pressed from the view, as with a gun.

Swap Sticks, letting go of R: the left stick's C-buttons are held back from the game (net_session.cpp, input_for_game) from the frame R is let go until no C-button is pressed, so C-Down held on the stick does not throw a grenade.

Swap Sticks, no weapon, R held: Z holds a grenade (look_aim.cpp, no_weapon_walk): C-Down is held with R for as long as Z is (the throw is on its release). While the look is being entered or left with R held, the stick's C-buttons are not passed on (C-Down with R started a grenade).

Swap Sticks, the grenade's look (kind 0x26): the stick-walking is not applied (it read the held C-Down as walking back and turned the view each frame).

Rocket launcher's look (kind 0x16): the frame has an extra pass of this machine's camera before its HUD pass, which draws the green overlay for the split screen's half; its scissors are emptied (net_view.cpp), so it is not drawn. This is done for every player using the rocket launcher, so the other player's screen does not show it either. The explosion (and the game's other screen-space sprites, such as sparkles) is emitted by the game as RT64's extended texture rectangle in the camera's own rectangle; in the rocket look those in this machine's camera's passes are moved down with the aim (the same shift as the HUD's aim) so the explosion lands where the missile does.

## Updating from the launcher (Windows)

When the Online screen finds a newer GitHub release that has a `.zip` file, an **Update now** button appears. It downloads the zip to
`%TEMP%\conker_update`, checks it against GitHub's SHA-256 when the release lists one, closes the game, and a small script (`update.bat`)
unpacks the zip over the game's folder (files named *.z64, *.n64, *.v64, *.rom are never touched) and starts the game again. The zip's files
can be at its top level or inside one folder. If anything fails, the script restarts the old game and opens `update.log`.
Releases must be non-draft, non-prerelease, tagged `v` + the version in `host/src/version.hpp`.
