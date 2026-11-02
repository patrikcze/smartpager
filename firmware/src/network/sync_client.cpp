// Network task: keeps Wi-Fi up, synchronizes time, runs the sync protocol and uploads
// read/ack events. Never touches LVGL; communicates through g_store / g_status / queue.
#include "sync_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <sys/time.h>

#include "app_state.h"
#include "messaging/policies.h"
#include "messaging/sync_protocol.h"
#include "storage/cache_store.h"

namespace net {
namespace {

TaskHandle_t task = nullptr;
volatile bool saveRequested = false;
uint32_t lastSaveMs = 0;
String caPem;  // loaded once; empty for http://
constexpr uint32_t kHttpTimeoutMs = 10000;
constexpr uint16_t kLongPollS = 20;      // server holds the request up to this long
constexpr size_t kMaxResponse = 48 * 1024;
constexpr size_t kPageLimit = 8;         // items per page; bounds the JSON size
constexpr size_t kMaxBody = sp::kBodyLen - 1;
constexpr size_t kEventsPerPost = 16;
constexpr int kMaxPagesPerSync = 20;

enum class Result { Ok, Network, Auth, Server, Protocol };

void setLink(sp::Link l, const char* err = "") {
    appLock();
    g_status.server = l;
    strlcpy(g_status.lastError, err, sizeof g_status.lastError);
    appUnlock();
}

bool clockValid() { return time(nullptr) > 1700000000; }

void persistIfNeeded(bool force) {
    if (!saveRequested) return;
    uint32_t now = millis();
    if (!force && now - lastSaveMs < 5000) return;  // bound flash wear: at most one write per 5 s
    saveRequested = false;
    appLock();
    size_t n = cache::snapshot(g_store);  // copy under the lock (fast, RAM only)
    appUnlock();
    lastSaveMs = now;
    if (!n || !cache::commit(n)) log_w("cache save failed");  // flash write without the lock
}

// One HTTP exchange. `body` null = GET. Response kept in `out` (bounded).
Result request(const char* method, const String& path, const String* body, uint32_t timeoutMs, String& out,
               int& code) {
    static WiFiClient plain;
    static WiFiClientSecure tls;
    const char* base = g_config.serverUrl;
    bool https = strncmp(base, "https://", 8) == 0;
    if (https) {
        if (caPem.isEmpty()) return Result::Protocol;  // never connect to TLS without a trust anchor
        tls.setCACert(caPem.c_str());
    }
    HTTPClient http;
    http.setReuse(true);
    http.setTimeout(timeoutMs);
    http.setConnectTimeout(5000);
    String url = String(base) + path;
    if (!(https ? http.begin(tls, url) : http.begin(plain, url))) return Result::Protocol;
    http.addHeader("Authorization", String("Bearer ") + g_config.token);
    http.addHeader("Accept", "application/json");
    if (body) http.addHeader("Content-Type", "application/json");
    code = body ? http.sendRequest(method, *body) : http.GET();
    Result r = Result::Ok;
    if (code <= 0) {
        r = Result::Network;
    } else if (code == 401 || code == 403) {
        r = Result::Auth;
    } else if (code != 200) {
        r = Result::Server;
    } else {
        int size = http.getSize();
        if (size > static_cast<int>(kMaxResponse)) r = Result::Protocol;
        else out = http.getString();
        if (out.length() > kMaxResponse) r = Result::Protocol;
    }
    http.end();
    return r;
}

const char* resultText(Result r, int code) {
    static char buf[40];
    switch (r) {
        case Result::Network: snprintf(buf, sizeof buf, "network error %d", code); break;
        case Result::Auth: return "token rejected";
        case Result::Server: snprintf(buf, sizeof buf, "HTTP %d", code); break;
        case Result::Protocol: return "bad response";
        default: return "";
    }
    return buf;
}

Result uploadEvents(uint32_t ackCursor, bool alwaysSendCursor) {
    while (true) {
        JsonDocument doc;
        size_t n;
        char ids[kEventsPerPost][17];
        appLock();
        n = sp::buildEventsBody(g_store, ackCursor, kEventsPerPost, doc);
        for (size_t i = 0; i < n; ++i) strlcpy(ids[i], g_store.pending(i).id, sizeof ids[i]);
        appUnlock();
        if (n == 0 && !alwaysSendCursor) return Result::Ok;
        JsonObject st = doc["status"].to<JsonObject>();
        st["fw"] = SMARTPAGER_VERSION;
        st["rssi"] = WiFi.RSSI();
        st["heap"] = ESP.getFreeHeap();
        st["psram"] = ESP.getFreePsram();
        st["uptime"] = millis() / 1000;
        st["cache_writes"] = cache::writes();
        String body;
        serializeJson(doc, body);
        String resp;
        int code = 0;
        Result r = request("POST", String("/api/v1/devices/") + g_config.deviceId + "/events", &body, kHttpTimeoutMs,
                           resp, code);
        if (r != Result::Ok) return r;
        // Every listed event (accepted, duplicate or unknown message) is final on the server.
        appLock();
        for (size_t i = 0; i < n; ++i) g_store.removePending(ids[i]);
        appUnlock();
        saveRequested = true;
        alwaysSendCursor = false;
        if (n < kEventsPerPost) return Result::Ok;
    }
}

Result syncOnce(bool allowWait) {
    static JsonDocument filter;
    if (filter.isNull()) sp::buildSyncFilter(filter);
    static sp::SyncItem scratch;  // 1.3 KB: keep off the task stack
    uint32_t firstFresh = 0;
    uint16_t freshCount = 0;
    bool anyApplied = false;

    for (int page = 0; page < kMaxPagesPerSync; ++page) {
        appLock();
        uint32_t cursor = g_store.cursor();
        String epoch = g_store.epoch();
        bool hasPending = g_store.pendingCount() > 0;
        appUnlock();
        uint16_t wait = (allowWait && page == 0 && g_config.longPoll && !hasPending) ? kLongPollS : 0;
        String path = String("/api/v1/devices/") + g_config.deviceId + "/sync?cursor=" + cursor +
                      "&limit=" + kPageLimit + "&max_body=" + kMaxBody + "&wait=" + wait + "&fw=" SMARTPAGER_VERSION;
        if (epoch.length()) path += "&epoch=" + epoch;
        String resp;
        int code = 0;
        Result r = request("GET", path, nullptr, kHttpTimeoutMs + wait * 1000u, resp, code);
        if (r != Result::Ok) {
            setLink(r == Result::Auth ? sp::Link::AuthError : r == Result::Network ? sp::Link::Offline : sp::Link::Error,
                    resultText(r, code));
            return r;
        }
        JsonDocument doc;
        if (deserializeJson(doc, resp, DeserializationOption::Filter(filter),
                            DeserializationOption::NestingLimit(4))) {
            setLink(sp::Link::Error, "invalid JSON");
            return Result::Protocol;
        }
        resp = String();  // release before applying
        sp::SyncPage pg;
        // Validate the whole page before touching the store; apply it under one lock.
        appLock();
        bool incremental = cursor > 0;
        sp::ParseError err = sp::parseSyncPage(doc, pg, scratch, [&](const sp::SyncItem& it) {
            if (!anyApplied && pg.reset) g_store.clearMessages();
            anyApplied = true;
            if (g_store.apply(it, incremental && !pg.reset) == sp::ApplyResult::Inserted && incremental &&
                !pg.reset && !it.read) {
                if (!firstFresh) firstFresh = it.id;
                ++freshCount;
            }
        });
        if (err == sp::ParseError::None) {
            if (pg.reset && pg.itemCount == 0) g_store.clearMessages();
            g_store.setCursor(pg.cursor, pg.epoch);
            g_status.lastSync = static_cast<uint32_t>(time(nullptr));
            g_status.server = sp::Link::Online;
            g_status.lastError[0] = 0;
        }
        appUnlock();
        if (err != sp::ParseError::None) {
            setLink(sp::Link::Error, err == sp::ParseError::Schema ? "unsupported schema" : "invalid sync data");
            return Result::Protocol;
        }
        if (pg.reset) log_i("sync: server requested reset");
        // Use server time until NTP has synchronized (keeps the clock useful behind firewalls).
        if (!clockValid() && pg.serverTime > 1700000000) {
            timeval tv{static_cast<time_t>(pg.serverTime), 0};
            settimeofday(&tv, nullptr);
        }
        if (pg.itemCount || pg.reset || cursor != pg.cursor) {
            saveRequested = true;
            persistIfNeeded(true);  // the cursor is acknowledged only after it is on flash
            Result er = uploadEvents(pg.cursor, true);
            if (er != Result::Ok) log_w("event upload failed: %s", resultText(er, 0));
        }
        if (!pg.hasMore) break;
        esp_task_wdt_reset();
    }
    if (freshCount) {
        NotifyMsg m{firstFresh, freshCount};
        xQueueSend(g_notifyQueue, &m, 0);
    }
    return Result::Ok;
}

void updateWifiStatus() {
    bool up = WiFi.status() == WL_CONNECTED;
    appLock();
    g_status.wifiConnected = up;
    g_status.rssi = up ? static_cast<int8_t>(WiFi.RSSI()) : 0;
    if (up) strlcpy(g_status.ip, WiFi.localIP().toString().c_str(), sizeof g_status.ip);
    g_status.timeValid = clockValid();
    if (!up && g_status.server == sp::Link::Online) g_status.server = sp::Link::Offline;
    appUnlock();
}

void taskMain(void*) {
    esp_task_wdt_add(nullptr);
    caPem = config::loadCaPem();
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(g_config.deviceId);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);  // credentials live in our own NVS namespace
    WiFi.begin(g_config.ssid, g_config.wifiPass);
    configTzTime(g_config.tz, "pool.ntp.org", "time.cloudflare.com");

    // Cap at 30 s: a pager should notice a recovered server quickly (measured on hardware:
    // with a 5 min cap, delivery after a 40 s outage took 52 s). 2 requests/min while down.
    sp::Backoff backoff(2000, 30 * 1000);
    uint32_t nextSyncMs = 0;
    uint32_t wifiDownSince = millis();
    bool lastOk = false;

    for (;;) {
        esp_task_wdt_reset();
        updateWifiStatus();
        persistIfNeeded(false);
        uint32_t now = millis();
        if (WiFi.status() != WL_CONNECTED) {
            setLink(sp::Link::Offline, "Wi-Fi down");
            if (now - wifiDownSince > 30000) {  // auto-reconnect gave up: kick it, at most every 30 s
                WiFi.disconnect();
                WiFi.begin(g_config.ssid, g_config.wifiPass);
                wifiDownSince = now;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        wifiDownSince = now;
        if (static_cast<int32_t>(now - nextSyncMs) >= 0) {
            if (!lastOk) setLink(sp::Link::Connecting);
            // Long-poll only when the previous request worked; after errors use plain polls.
            Result r = syncOnce(lastOk);
            if (r == Result::Ok) {
                appLock();
                uint32_t cursor = g_store.cursor();
                appUnlock();
                r = uploadEvents(cursor, false);
            }
            lastOk = r == Result::Ok;
            if (lastOk) {
                backoff.reset();
                // After a long poll returns we can immediately poll again; otherwise wait.
                nextSyncMs = millis() + (g_config.longPoll ? 250 : g_config.pollSeconds * 1000u);
            } else {
                uint32_t d = r == Result::Auth ? 5 * 60 * 1000 : backoff.next(esp_random());
                nextSyncMs = millis() + d;
                log_w("sync failed (%d), retry in %u ms", static_cast<int>(r), static_cast<unsigned>(d));
            }
        }
        uint32_t waitMs = static_cast<int32_t>(nextSyncMs - millis()) > 0 ? nextSyncMs - millis() : 0;
        // kick() wakes us early: user action or "sync now"
        if (waitMs && ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs > 1000 ? 1000 : waitMs)))
            nextSyncMs = millis();  // user action or "sync now": also cuts a retry backoff short
    }
}

}  // namespace

void start() { xTaskCreatePinnedToCore(taskMain, "net", 12 * 1024, nullptr, 2, &task, 0); }

void kick() {
    if (task) xTaskNotifyGive(task);
}

void requestSave() { saveRequested = true; }

}  // namespace net
