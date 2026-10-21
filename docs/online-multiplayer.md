# Online multiplayer

The game's local multiplayer (the arena modes, 2 to 4 players) played over a network, each player with a full-screen view
instead of a split screen. Everything is deterministic lockstep: the machines exchange only controller inputs, each runs
the whole game, and the host sends a snapshot to put a machine right if its random numbers ever drift apart.

There is no lobby screen yet: a game is set up with environment variables, then started from the game's own multiplayer menu.

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

## Known limits

- During a match the Start button does not pause (the game's pause stops everything, and every machine has to go on together). It opens a small menu (Continue, Quit) instead, on that player's screen only: the match goes on behind it, and that player's character stands still. Quit closes that player's game; the others see "Player N left the match". The menu is plain text for now, not the game's own pause artwork, and has no stats row.
- A machine that cannot be reached for a while is dropped: the others see "Player N left the match" and go on, and a joiner whose host is gone sees a notice and goes on offline.
- Audio is the local listener's own, positional sound is not yet per player.
- Tested with 2 players; 3 and 4 are untested.
