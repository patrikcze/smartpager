#pragma once

#include <stdint.h>

namespace net {
// Starts the network task (Wi-Fi, NTP, sync loop) pinned to core 0.
void start();
// Wake the task now: after a user action queued an event, or "Sync now".
void kick();
// Persists the cache if it changed (debounced); called from the network task and on demand.
void requestSave();
}  // namespace net
