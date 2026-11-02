#pragma once

namespace portal {
// Setup mode: starts a WPA2 access point with a random per-session password (shown on the
// display) and a small configuration page at http://192.168.4.1. Never returns: saving
// restarts the device.
[[noreturn]] void run();
}  // namespace portal
