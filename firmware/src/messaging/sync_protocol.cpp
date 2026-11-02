#include "sync_protocol.h"

#include <string.h>

namespace sp {

void buildSyncFilter(JsonDocument& f) {
    f.clear();
    for (const char* k : {"schema", "epoch", "server_time", "reset", "cursor", "has_more", "unread"}) f[k] = true;
    JsonObject it = f["items"].add<JsonObject>();
    for (const char* k : {"id", "seq", "deleted", "src", "from", "subj", "body", "trunc", "pri", "ts", "read", "ack"})
        it[k] = true;
}

static bool isUint(JsonVariantConst v) { return v.is<uint32_t>(); }

static bool optString(JsonVariantConst v, char* dst, size_t cap) {
    if (v.isNull()) { dst[0] = 0; return true; }
    if (!v.is<const char*>()) return false;
    copyUtf8(dst, cap, v.as<const char*>());
    return true;
}

static bool optUint(JsonVariantConst v, uint32_t& dst) {
    if (v.isNull()) { dst = 0; return true; }
    if (!isUint(v)) return false;
    dst = v.as<uint32_t>();
    return true;
}

bool parseItem(JsonObjectConst o, SyncItem& out) {
    if (o.isNull() || !isUint(o["id"]) || !isUint(o["seq"])) return false;
    out = SyncItem{};
    out.id = o["id"];
    out.seq = o["seq"];
    if (out.id == 0) return false;
    out.deleted = o["deleted"] | false;
    if (out.deleted) return true;
    out.truncated = o["trunc"] | false;
    uint32_t pri = 1;
    if (!optUint(o["pri"], pri) || pri > 3) return false;
    out.pri = static_cast<uint8_t>(pri);
    if (!(optUint(o["ts"], out.ts) && optUint(o["read"], out.read) && optUint(o["ack"], out.ack) &&
          optString(o["src"], out.src, sizeof out.src) && optString(o["from"], out.sender, sizeof out.sender) &&
          optString(o["subj"], out.subject, sizeof out.subject) && optString(o["body"], out.body, sizeof out.body)))
        return false;
    sanitizeText(out.sender);
    sanitizeText(out.subject);
    sanitizeText(out.body);
    return true;
}

ParseError parseHeader(JsonDocument& doc, SyncPage& page) {
    page = SyncPage{};
    if (!doc.is<JsonObject>()) return ParseError::Json;
    if (!doc["schema"].is<int>()) return ParseError::Schema;
    page.schema = doc["schema"];
    if (page.schema != kSyncSchema) return ParseError::Schema;
    const char* epoch = doc["epoch"];
    if (!epoch || !*epoch || strlen(epoch) >= kEpochLen) return ParseError::Field;
    strcpy(page.epoch, epoch);
    if (!isUint(doc["cursor"]) || !doc["items"].is<JsonArray>()) return ParseError::Field;
    page.cursor = doc["cursor"];
    page.serverTime = doc["server_time"] | 0u;
    page.reset = doc["reset"] | false;
    page.hasMore = doc["has_more"] | false;
    page.unread = doc["unread"] | 0;
    return ParseError::None;
}

size_t buildEventsBody(const MessageStore& store, uint32_t ackCursor, size_t max, JsonDocument& out) {
    out.clear();
    out["ack_cursor"] = ackCursor;
    JsonArray evs = out["events"].to<JsonArray>();
    size_t n = store.pendingCount() < max ? store.pendingCount() : max;
    for (size_t i = 0; i < n; ++i) {
        const PendingEvent& e = store.pending(i);
        JsonObject o = evs.add<JsonObject>();
        o["id"] = e.id;
        o["type"] = e.type == EventType::Ack ? "ack" : "read";
        o["message_id"] = e.messageId;
        if (e.at > 1600000000u) o["at"] = e.at;  // only send when the clock was synchronized
    }
    return n;
}

}  // namespace sp
