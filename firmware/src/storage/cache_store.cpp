#include "cache_store.h"

#include <Arduino.h>
#include <LittleFS.h>

namespace cache {
namespace {
constexpr const char* kPath = "/inbox.bin";
constexpr const char* kTmp = "/inbox.tmp";
bool mounted = false;
uint32_t writeCount = 0;
uint8_t* buf = nullptr;  // PSRAM scratch for (de)serialization

bool ensureBuf() {
    if (!buf) buf = static_cast<uint8_t*>(ps_malloc(sp::MessageStore::serializedSizeMax()));
    return buf != nullptr;
}
}  // namespace

bool begin() {
    // The 16 MB partition table names the data partition "spiffs"; it is formatted as
    // LittleFS on first use (this erases whatever the previous firmware stored there).
    mounted = LittleFS.begin(true, "/littlefs", 5, "spiffs");
    if (!mounted) log_e("LittleFS mount failed; running without persistent cache");
    return mounted;
}

static bool loadFile(const char* path, sp::MessageStore& store) {
    if (!LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    size_t size = f.size();
    size_t n = size <= sp::MessageStore::serializedSizeMax() ? f.read(buf, size) : 0;
    f.close();
    if (n == 0 || !store.deserialize(buf, n)) {
        log_w("cache %s invalid (%u bytes)", path, static_cast<unsigned>(size));
        LittleFS.remove(path);
        return false;
    }
    return true;
}

bool load(sp::MessageStore& store) {
    if (!mounted || !ensureBuf()) return false;
    if (loadFile(kPath, store)) return true;
    // power lost between removing the old file and the rename: the complete temp file wins
    if (loadFile(kTmp, store)) {
        LittleFS.rename(kTmp, kPath);
        return true;
    }
    return false;  // store is empty, cursor 0: full resync
}

size_t snapshot(const sp::MessageStore& store) {
    if (!mounted || !ensureBuf()) return 0;
    return store.serialize(buf, sp::MessageStore::serializedSizeMax());
}

bool commit(size_t n) {
    if (!mounted || !buf || n == 0) return false;
    File f = LittleFS.open(kTmp, "w");
    if (!f) return false;
    size_t w = f.write(buf, n);
    f.close();
    if (w != n) {
        LittleFS.remove(kTmp);
        return false;
    }
    LittleFS.remove(kPath);  // Arduino's LittleFS rename does not overwrite; load() recovers kTmp
    bool ok = LittleFS.rename(kTmp, kPath);
    if (ok) ++writeCount;
    return ok;
}

void erase() {
    if (!mounted) return;
    LittleFS.remove(kPath);
    LittleFS.remove(kTmp);
}

uint32_t writes() { return writeCount; }

}  // namespace cache
