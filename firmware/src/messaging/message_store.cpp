#include "message_store.h"

#include <stdio.h>
#include <string.h>

namespace sp {

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void copyUtf8(char* dst, size_t cap, const char* src) {
    if (cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strnlen(src, cap);
    if (n >= cap) {
        n = cap - 1;
        // back off to the start of a code point (continuation bytes are 10xxxxxx)
        while (n > 0 && (static_cast<uint8_t>(src[n]) & 0xC0) == 0x80) --n;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

void sanitizeText(char* s) {
    const uint8_t* r = reinterpret_cast<const uint8_t*>(s);
    char* w = s;
    while (*r) {
        uint8_t c = *r;
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        bool valid = len > 0;
        for (size_t i = 1; valid && i < len; ++i) valid = (r[i] & 0xC0) == 0x80;
        if (!valid) { ++r; continue; }  // skip a malformed byte
        bool drop = len == 4 ||
                    (len == 3 && r[0] == 0xEF && r[1] == 0xB8 && (r[2] == 0x8E || r[2] == 0x8F)) ||  // U+FE0E/F
                    (len == 3 && r[0] == 0xE2 && r[1] == 0x80 && r[2] == 0x8D);                     // U+200D
        if (!drop) {
            memmove(w, r, len);
            w += len;
        }
        r += len;
    }
    *w = 0;
}

void MessageStore::clear() {
    count_ = 0;
    pendingCount_ = 0;
    cursor_ = 0;
    epoch_[0] = 0;
    bump();
}

void MessageStore::clearMessages() {
    count_ = 0;
    cursor_ = 0;
    bump();
}

int MessageStore::indexOf(uint32_t id) const {
    for (size_t i = 0; i < count_; ++i)
        if (msgs_[i].id == id) return static_cast<int>(i);
    return -1;
}

uint16_t MessageStore::unreadCount() const {
    uint16_t n = 0;
    for (size_t i = 0; i < count_; ++i)
        if (!msgs_[i].readAt) ++n;
    return n;
}

void MessageStore::removeAt(size_t i) {
    memmove(&msgs_[i], &msgs_[i + 1], (count_ - i - 1) * sizeof(Message));
    --count_;
}

// newest first: ts desc, id desc
size_t MessageStore::insertPos(uint32_t ts, uint32_t id) const {
    size_t i = 0;
    while (i < count_ && (msgs_[i].ts > ts || (msgs_[i].ts == ts && msgs_[i].id > id))) ++i;
    return i;
}

ApplyResult MessageStore::apply(const SyncItem& it, bool incremental) {
    int idx = indexOf(it.id);
    if (it.deleted) {
        if (idx < 0) return ApplyResult::Ignored;
        removeAt(static_cast<size_t>(idx));
        bump();
        return ApplyResult::Deleted;
    }
    if (idx >= 0) {
        Message& m = msgs_[idx];
        // Read/ack only move forward: a local action not yet confirmed by the server
        // must survive a sync that still reports the old state.
        m.seq = it.seq;
        m.pri = it.pri;
        if (it.read && !m.readAt) m.readAt = it.read;
        if (it.ack && !m.ackAt) m.ackAt = it.ack;
        if (m.readAt) m.flags &= ~kFresh;
        copyUtf8(m.subject, sizeof m.subject, it.subject);
        copyUtf8(m.sender, sizeof m.sender, it.sender);
        copyUtf8(m.body, sizeof m.body, it.body);
        m.flags = (m.flags & ~kTruncated) | (it.truncated ? kTruncated : 0);
        bump();
        return ApplyResult::Updated;
    }
    size_t pos = insertPos(it.ts, it.id);
    if (count_ == kMaxMessages) {
        if (pos >= count_) return ApplyResult::Ignored;  // older than everything we keep
        // evict the oldest read message, or the oldest message if all are unread
        size_t victim = count_ - 1;
        for (size_t i = count_; i-- > 0;)
            if (msgs_[i].readAt) { victim = i; break; }
        removeAt(victim);
        if (victim < pos) --pos;
    }
    memmove(&msgs_[pos + 1], &msgs_[pos], (count_ - pos) * sizeof(Message));
    ++count_;
    Message& m = msgs_[pos];
    memset(&m, 0, sizeof m);
    m.id = it.id;
    m.seq = it.seq;
    m.ts = it.ts;
    m.readAt = it.read;
    m.ackAt = it.ack;
    m.pri = it.pri > 3 ? 3 : it.pri;
    m.flags = (it.truncated ? kTruncated : 0) | ((incremental && !it.read) ? kFresh : 0);
    copyUtf8(m.src, sizeof m.src, it.src);
    copyUtf8(m.sender, sizeof m.sender, it.sender);
    copyUtf8(m.subject, sizeof m.subject, it.subject);
    copyUtf8(m.body, sizeof m.body, it.body);
    bump();
    return ApplyResult::Inserted;
}

void MessageStore::setCursor(uint32_t cursor, const char* epoch) {
    cursor_ = cursor;
    copyUtf8(epoch_, sizeof epoch_, epoch);
    bump();
}

bool MessageStore::queue(EventType t, uint32_t id, uint32_t now, RandomFn rnd) {
    if (pendingCount_ == kMaxPending) {
        // Bounded: drop the oldest. The local state is still correct; only the server
        // misses that one transition, which the user can redo from the admin UI.
        memmove(&pending_[0], &pending_[1], (kMaxPending - 1) * sizeof(PendingEvent));
        --pendingCount_;
    }
    PendingEvent& e = pending_[pendingCount_++];
    snprintf(e.id, sizeof e.id, "%08x%08x", static_cast<unsigned>(rnd()), static_cast<unsigned>(rnd()));
    e.type = t;
    e.messageId = id;
    e.at = now;
    return true;
}

bool MessageStore::markRead(uint32_t id, uint32_t now, RandomFn rnd) {
    int idx = indexOf(id);
    if (idx < 0) return false;
    Message& m = msgs_[idx];
    m.flags &= ~kFresh;
    if (m.readAt) { bump(); return false; }
    m.readAt = now ? now : 1;
    queue(EventType::Read, id, now, rnd);
    bump();
    return true;
}

bool MessageStore::markAck(uint32_t id, uint32_t now, RandomFn rnd) {
    int idx = indexOf(id);
    if (idx < 0) return false;
    Message& m = msgs_[idx];
    if (m.ackAt) return false;
    m.ackAt = now ? now : 1;
    if (!m.readAt) m.readAt = m.ackAt;  // server applies the same rule
    m.flags &= ~kFresh;
    queue(EventType::Ack, id, now, rnd);
    bump();
    return true;
}

void MessageStore::clearFresh(uint32_t id) {
    int idx = indexOf(id);
    if (idx >= 0 && (msgs_[idx].flags & kFresh)) {
        msgs_[idx].flags &= ~kFresh;
        bump();
    }
}

void MessageStore::dropPending(size_t n) {
    if (n >= pendingCount_) {
        pendingCount_ = 0;
    } else {
        memmove(&pending_[0], &pending_[n], (pendingCount_ - n) * sizeof(PendingEvent));
        pendingCount_ -= n;
    }
    bump();
}

void MessageStore::removePending(const char* id) {
    for (size_t i = 0; i < pendingCount_; ++i) {
        if (strcmp(pending_[i].id, id) == 0) {
            memmove(&pending_[i], &pending_[i + 1], (pendingCount_ - i - 1) * sizeof(PendingEvent));
            --pendingCount_;
            bump();
            return;
        }
    }
}

// ---------------------------------------------------------------- persistence

namespace {
constexpr uint32_t kMagic = 0x31435053;  // "SPC1"
struct Header {
    uint32_t magic;
    uint16_t msgSize;
    uint16_t evSize;
    uint32_t count;
    uint32_t pendingCount;
    uint32_t cursor;
    char epoch[kEpochLen];
    uint8_t pad[3];
    uint32_t crc;  // over everything after the header
};
}  // namespace

size_t MessageStore::serializedSizeMax() {
    return sizeof(Header) + kMaxMessages * sizeof(Message) + kMaxPending * sizeof(PendingEvent);
}

size_t MessageStore::serialize(uint8_t* buf, size_t cap) const {
    size_t need = sizeof(Header) + count_ * sizeof(Message) + pendingCount_ * sizeof(PendingEvent);
    if (cap < need) return 0;
    Header h{};
    h.magic = kMagic;
    h.msgSize = sizeof(Message);
    h.evSize = sizeof(PendingEvent);
    h.count = count_;
    h.pendingCount = pendingCount_;
    h.cursor = cursor_;
    memcpy(h.epoch, epoch_, kEpochLen);
    uint8_t* p = buf + sizeof(Header);
    memcpy(p, msgs_, count_ * sizeof(Message));
    p += count_ * sizeof(Message);
    memcpy(p, pending_, pendingCount_ * sizeof(PendingEvent));
    h.crc = crc32(buf + sizeof(Header), need - sizeof(Header));
    memcpy(buf, &h, sizeof h);
    return need;
}

bool MessageStore::deserialize(const uint8_t* buf, size_t len) {
    count_ = 0;
    pendingCount_ = 0;
    cursor_ = 0;
    epoch_[0] = 0;
    bump();
    Header h;
    if (len < sizeof h) return false;
    memcpy(&h, buf, sizeof h);
    if (h.magic != kMagic || h.msgSize != sizeof(Message) || h.evSize != sizeof(PendingEvent) ||
        h.count > kMaxMessages || h.pendingCount > kMaxPending)
        return false;
    size_t need = sizeof h + h.count * sizeof(Message) + h.pendingCount * sizeof(PendingEvent);
    if (len != need || crc32(buf + sizeof h, need - sizeof h) != h.crc) return false;
    if (h.epoch[kEpochLen - 1] != 0) return false;
    memcpy(msgs_, buf + sizeof h, h.count * sizeof(Message));
    memcpy(pending_, buf + sizeof h + h.count * sizeof(Message), h.pendingCount * sizeof(PendingEvent));
    for (size_t i = 0; i < h.count; ++i) {  // never trust string termination from flash
        Message& m = msgs_[i];
        m.src[kSrcLen - 1] = m.sender[kSenderLen - 1] = m.subject[kSubjectLen - 1] = m.body[kBodyLen - 1] = 0;
    }
    for (size_t i = 0; i < h.pendingCount; ++i) pending_[i].id[sizeof pending_[i].id - 1] = 0;
    count_ = h.count;
    pendingCount_ = h.pendingCount;
    cursor_ = h.cursor;
    memcpy(epoch_, h.epoch, kEpochLen);
    bump();
    return true;
}

}  // namespace sp
