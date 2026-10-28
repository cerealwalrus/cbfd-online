// Rumble: off for good (0.10.4). The game still turns each port's Rumble Pak on and off (the runtime's rumble callback,
// set), and nothing is sent to the controllers: no motor runs, and the settings for it (strength, motor, style) are gone
// from the General tab. (Until 0.10.3 every VI sent each port's rumble to its controller here, with those settings.)

#include "conker.hpp"

void conker::rumble::set(int port, bool on) {
    (void)port;
    (void)on;
}

void conker::rumble::update() {
}
