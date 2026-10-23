#pragma once

// The online build's version. Both players of a match need the same one (the handshake compares them), and the launcher's update check
// compares it with the newest release published on GitHub (update_check.cpp). Bump it for every release, and name the release's tag the
// same with a "v" in front (v0.7.10).
#define CONKER_ONLINE_VERSION "0.9.28"

// Where the releases are published (owner/repo). CONKER_UPDATE_REPO overrides it.
#define CONKER_UPDATE_REPO "cerealwalrus/cbfd-online"
