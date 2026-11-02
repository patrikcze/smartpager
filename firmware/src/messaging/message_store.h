// Bounded in-memory inbox with sync-cursor and pending-event bookkeeping.
// Pure C++ (no Arduino dependencies) so it is unit-tested on the host.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sp {

constexpr size_t kMaxMessages = 40;
constexpr size_t kMaxPending = 32;
constexpr size_t kSrcLen = 12;
constexpr size_t kSenderLen = 64;
constexpr size_t kSubjectLen = 160;
constexpr size_t kBodyLen = 1024;  // bytes incl. NUL; server truncates bodies to fit (max_body)
constexpr size_t kEpochLen = 17;

enum MsgFlags : uint8_t {
    kTruncated = 1 << 0,  // server sent a shortened body
    kFresh = 1 << 1,      // arrived via incremental sync and not yet viewed: highlight it
};

struct Message {
    uint32_t id;
    uint32_t seq;
    uint32_t ts;      // created, unix seconds UTC
    uint32_t readAt;  // 0 = unread
    uint32_t ackAt;   // 0 = not acknowledged
    uint8_t pri;      // 0 low .. 3 critical
    uint8_t flags;
    char src[kSrcLen];
    char sender[kSenderLen];
    char subject[kSubjectLen];
    char body[kBodyLen];
};

// One change from GET /devices/{id}/sync, already copied into bounded buffers.
struct SyncItem {
    uint32_t id = 0;
    uint32_t seq = 0;
    bool deleted = false;
    bool truncated = false;
    uint8_t pri = 1;
    uint32_t ts = 0, read = 0, ack = 0;
    char src[kSrcLen] = {};
    char sender[kSenderLen] = {};
    char subject[kSubjectLen] = {};
    char body[kBodyLen] = {};
};

enum class EventType : uint8_t { Read = 0, Ack = 1 };

struct PendingEvent {
    char id[17];  // 64-bit random, hex: unique across reboots and cache loss
    EventType type;
    uint32_t messageId;
    uint32_t at;
};

enum class ApplyResult { Inserted, Updated, Deleted, Ignored };

using RandomFn = uint32_t (*)();

class MessageStore {
public:
    // `slots` must hold kMaxMessages messages (on the device it lives in PSRAM).
    explicit MessageStore(Message* slots) : msgs_(slots) {}

    void clear();          // everything, including cursor and pending events
    void clearMessages();  // used when the server signals a reset; keeps pending events

    // Apply one sync change. `incremental` = false for reset/initial snapshots
    // (those never produce "new message" highlights).
    ApplyResult apply(const SyncItem& item, bool incremental);
    void setCursor(uint32_t cursor, const char* epoch);
    uint32_t cursor() const { return cursor_; }
    const char* epoch() const { return epoch_; }

    size_t count() const { return count_; }
    const Message& at(size_t i) const { return msgs_[i]; }  // newest first
    int indexOf(uint32_t id) const;
    uint16_t unreadCount() const;

    // Local user actions; queue an event for the server. Return false if nothing changed.
    bool markRead(uint32_t id, uint32_t now, RandomFn rnd);
    bool markAck(uint32_t id, uint32_t now, RandomFn rnd);
    void clearFresh(uint32_t id);

    size_t pendingCount() const { return pendingCount_; }
    const PendingEvent& pending(size_t i) const { return pending_[i]; }
    void dropPending(size_t n);  // remove the oldest n
    // Remove a confirmed event by id (robust if the queue changed while a request was in flight).
    void removePending(const char* id);

    // Monotonic change counter: the UI redraws only when it changes.
    uint32_t version() const { return version_; }

    // Persistence (fixed binary layout + CRC32). `buf` must be >= serializedSizeMax().
    static size_t serializedSizeMax();
    size_t serialize(uint8_t* buf, size_t cap) const;
    bool deserialize(const uint8_t* buf, size_t len);  // false => corrupt; store left empty

private:
    void removeAt(size_t i);
    size_t insertPos(uint32_t ts, uint32_t id) const;
    bool queue(EventType t, uint32_t id, uint32_t now, RandomFn rnd);
    void bump() { ++version_; }

    Message* msgs_;
    size_t count_ = 0;
    PendingEvent pending_[kMaxPending] = {};
    size_t pendingCount_ = 0;
    uint32_t cursor_ = 0;
    char epoch_[kEpochLen] = {};
    uint32_t version_ = 1;
};

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0);

// Copy at most cap-1 bytes without splitting a UTF-8 sequence; always NUL-terminates.
void copyUtf8(char* dst, size_t cap, const char* src);

// In place: drops code points the device fonts cannot draw and that would otherwise
// render as boxes (emoji and other astral-plane characters, ZWJ, variation selectors)
// as well as malformed UTF-8. Characters such as Czech diacritics are kept.
void sanitizeText(char* s);

}  // namespace sp
