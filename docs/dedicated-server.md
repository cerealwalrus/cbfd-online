# Dedicated server (0.9.2)

`ConkerRecomp --dedicated --rom <baserom.us.z64> [--port 47000] [--players 2] [--delay 3]`

Runs the game headless (no window, no sound card) as the online host **without a player slot**.
Everyone, including whoever joins first, is a client and plays through the launcher as usual
(Online tab → host address, port).

The network is **UDP** (from 0.9.2: reliable, ordered messages on ENet, `host/third_party/enet`). The server's port (47000 by default) must be forwarded on its router as **UDP**, and allowed for UDP in its firewall; a forwarded TCP port does nothing. IPv4 only for now.

* The first player to join takes slot 0 and is the **leader**: slot 0 is the player the game's menus
  listen to, so the leader picks the multiplayer mode and map. The lobby shows "(leader: picks the match)".
* The server keeps the game running even if the leader's PC is slow; the clients stay in lockstep with it.
* The server writes `netplay_server.txt` (set `CONKER_NET_LOG` to change it) with `[server] frame ...`
  lines every 600 frames (frames a second on average — it should sit near 30/60 depending on the game).
* The server's game starts when the server does and keeps running whether or not anyone is connected; it only goes offline when its operator closes the application. (A supervisor process starts it again if it ever crashes.)
* Joining a running game: a player who joins is told the game is already running, its game boots and idles, then it asks the server for a snapshot of the running game (the same snapshot a resync uses), loads it and replays the few frames since. From the first frame after the snapshot the player takes part (its controller is "plugged in" from then on, like a controller plugged in during play). The server waits for it from that frame, so a player still loading its snapshot is waited for (the game stalls for everyone meanwhile; several players joining at once on one PC can take many seconds), but one whose game never answers is dropped; one that never takes the snapshot in 90 s is let go.
* From the launcher: Online screen → **Start dedicated server** (uses the port / players / delay shown; the server stops with the launcher or with **Stop dedicated server**). Join it with 127.0.0.1 from the same PC.
* Windows only for the supervisor/launcher button; elsewhere run `--dedicated` under a loop.
* Both ends must be the same version (0.9.2 or later).

The launcher's menus answer the mouse and keyboard only (not the game controller) until a game is running, so a pad press in a game window on the same PC cannot press the launcher's Stop button.

Tested so far: a server with 1 to 4 clients joining and leaving a running game, all on one PC (30 frames a second, no desyncs reported). Known limits: one session per process (the supervisor restarts it); if the leader leaves, nobody drives the menus until they return; several clients on one PC joining at the same moment load slowly; the real-time pacing and the virtual audio clock of the headless server are not verified over a real network.
