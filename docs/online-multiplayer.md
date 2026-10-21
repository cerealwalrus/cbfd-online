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
