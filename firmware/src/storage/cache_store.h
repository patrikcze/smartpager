// Crash-safe persistence of the MessageStore (messages + sync cursor + pending events)
// on LittleFS. The cursor and the messages are written in one file so they can never
// disagree after a power loss.
#pragma once

#include "messaging/message_store.h"

namespace cache {
bool begin();
// Loads the cache into `store`. On a missing or corrupt file the store is empty and the
// device performs a full resync (cursor 0).
bool load(sp::MessageStore& store);
// Two-step save so the caller can hold its lock only while copying:
// snapshot() serializes into an internal PSRAM buffer (returns size, 0 on error);
// commit() writes that buffer to a temp file and renames it over the previous cache.
size_t snapshot(const sp::MessageStore& store);
bool commit(size_t n);
void erase();
uint32_t writes();
}  // namespace cache
