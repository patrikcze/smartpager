#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <stdint.h>

namespace net {
// Starts the network task (Wi-Fi, NTP, sync loop) pinned to core 0.
void start();
// Wake the task now: after a user action queued an event, or "Sync now".
void kick();
// Persists the cache if it changed (debounced); called from the network task and on demand.
void requestSave();
// Network task only: write the cache to flash immediately (before a firmware restart).
void saveNow();
// From the UI: check for a signed firmware update, or install the one already found.
void requestOta();
// Network task only: prepares an authenticated request to the configured server
// (custom CA or built-in Let's Encrypt roots for https).
bool beginRequest(HTTPClient& http, const String& path, uint32_t timeoutMs);
}  // namespace net
