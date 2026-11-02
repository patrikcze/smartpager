// Parsing of the server's sync response (schema 1) and building of the events payload.
// Everything coming from the network is treated as untrusted: types are checked,
// strings are bounded, and any structural problem rejects the whole page so the
// cursor is never advanced past data that was not applied.
#pragma once

#include <ArduinoJson.h>

#include "message_store.h"

namespace sp {

constexpr int kSyncSchema = 1;

struct SyncPage {
    int schema = 0;
    char epoch[kEpochLen] = {};
    uint32_t serverTime = 0;
    bool reset = false;
    uint32_t cursor = 0;
    bool hasMore = false;
    uint16_t unread = 0;
    size_t itemCount = 0;
};

enum class ParseError { None, Json, Schema, Field };

// Filter applied while deserializing so unknown fields never consume memory.
void buildSyncFilter(JsonDocument& filter);

// Validates the header fields and every item. Items are delivered through `onItem` only
// after the whole document validated, so a bad item never leaves a partial page applied.
template <typename OnItem>
ParseError parseSyncPage(JsonDocument& doc, SyncPage& page, SyncItem& scratch, OnItem onItem);

// Copies pending events (up to `max`) into the request body for POST /events.
size_t buildEventsBody(const MessageStore& store, uint32_t ackCursor, size_t max, JsonDocument& out);

// ---------------------------------------------------------------- implementation

bool parseItem(JsonObjectConst o, SyncItem& out);
ParseError parseHeader(JsonDocument& doc, SyncPage& page);

template <typename OnItem>
ParseError parseSyncPage(JsonDocument& doc, SyncPage& page, SyncItem& scratch, OnItem onItem) {
    ParseError err = parseHeader(doc, page);
    if (err != ParseError::None) return err;
    JsonArrayConst items = doc["items"].as<JsonArrayConst>();
    for (JsonObjectConst o : items)
        if (!parseItem(o, scratch)) return ParseError::Field;
    page.itemCount = items.size();
    for (JsonObjectConst o : items) {
        parseItem(o, scratch);
        onItem(scratch);
    }
    return ParseError::None;
}

}  // namespace sp
