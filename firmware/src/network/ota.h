// Signed over-the-air firmware updates. All functions run in the network task.
#pragma once

namespace ota {
// True when the firmware was built with an OTA public key (tools/ota.py keygen).
bool enabled();
// Fetch the manifest and verify its signature; publishes the result in g_status.
void check();
// User request from Settings: install the verified update if one is pending, else check.
void checkOrInstall();
}  // namespace ota
