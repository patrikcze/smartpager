// Signed OTA: the manifest's ECDSA P-256 signature is verified against the public key compiled
// into this firmware before an update is even offered; the downloaded image must then match the
// signed SHA-256 before the boot partition is switched. A compromised server can therefore
// withhold updates but cannot install anything. The install is only started by the user
// (hold KEY on Settings → Update), never automatically.
#include "ota.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include "app_state.h"
#include "messaging/ota_manifest.h"
#include "network/sync_client.h"

#if __has_include("ota_pubkey.h")
#include "ota_pubkey.h"
#endif

namespace ota {
namespace {

sp::OtaManifest pending;  // verified and newer than the running firmware
bool havePending = false;

void setStatus(const char* note, int progress = -1, const char* version = nullptr) {
    appLock();
    strlcpy(g_status.updateNote, note, sizeof g_status.updateNote);
    g_status.updateProgress = static_cast<int8_t>(progress);
    if (version) strlcpy(g_status.updateVersion, version, sizeof g_status.updateVersion);
    appUnlock();
}

bool verifySignature(const sp::OtaManifest& m) {
#ifdef SMARTPAGER_OTA_PUBKEY
    char msg[160];
    size_t n = sp::otaSignedMessage(m, msg, sizeof msg);
    if (!n) return false;
    uint8_t hash[32];
    if (mbedtls_sha256_ret(reinterpret_cast<const uint8_t*>(msg), n, hash, 0) != 0) return false;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    static const char kPub[] = SMARTPAGER_OTA_PUBKEY;
    int rc = mbedtls_pk_parse_public_key(&pk, reinterpret_cast<const uint8_t*>(kPub), sizeof kPub);
    if (rc == 0 && !mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECDSA)) rc = -1;
    if (rc == 0) rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof hash, m.signature, m.signatureLen);
    mbedtls_pk_free(&pk);
    return rc == 0;
#else
    (void)m;
    return false;
#endif
}

String devicePath(const char* suffix) { return String("/api/v1/devices/") + g_config.deviceId + suffix; }

}  // namespace

bool enabled() {
#ifdef SMARTPAGER_OTA_PUBKEY
    return true;
#else
    return false;
#endif
}

void check() {
    if (!enabled()) {
        setStatus("Not configured");
        return;
    }
    HTTPClient http;
    if (!net::beginRequest(http, devicePath("/firmware"), 10000)) return setStatus("Check failed");
    int code = http.GET();
    String body = code == 200 && http.getSize() < 1024 ? http.getString() : String();
    http.end();
    if (code == 404) {
        havePending = false;
        return setStatus("Up to date", -1, "");
    }
    JsonDocument doc;
    sp::OtaManifest m;
    if (code != 200 || deserializeJson(doc, body) || !sp::parseOtaManifest(doc, m)) return setStatus("Check failed");
    int cmp = 0;
    if (!sp::compareVersions(m.version, SMARTPAGER_VERSION, cmp) || cmp <= 0) {
        havePending = false;
        return setStatus("Up to date", -1, "");
    }
    if (!verifySignature(m)) {
        havePending = false;
        log_w("ota: manifest for %s has an invalid signature; ignored", m.version);
        return setStatus("Bad signature", -1, "");
    }
    pending = m;
    havePending = true;
    log_i("ota: verified update %s available (%u bytes)", m.version, static_cast<unsigned>(m.size));
    setStatus("Available", -1, m.version);
}

void checkOrInstall() {
    if (!havePending) return check();
    const sp::OtaManifest m = pending;
    setStatus("Downloading", 0);
    HTTPClient http;
    if (!net::beginRequest(http, devicePath("/firmware/image"), 15000)) return setStatus("Download failed");
    int code = http.GET();
    if (code != 200 || http.getSize() != static_cast<int>(m.size)) {
        http.end();
        return setStatus("Download failed");
    }
    if (!Update.begin(m.size, U_FLASH)) {
        http.end();
        log_w("ota: Update.begin failed: %s", Update.errorString());
        return setStatus("No space");
    }
    WiFiClient* stream = http.getStreamPtr();
    static uint8_t buf[4096];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    uint32_t done = 0, lastData = millis();
    int lastPct = -1;
    bool ok = true;
    while (done < m.size) {
        esp_task_wdt_reset();
        size_t avail = stream->available();
        if (!avail) {
            if (!http.connected() || millis() - lastData > 15000) { ok = false; break; }
            delay(5);
            continue;
        }
        size_t want = min(min(avail, sizeof buf), static_cast<size_t>(m.size - done));
        int r = stream->readBytes(buf, want);
        if (r <= 0) { ok = false; break; }
        lastData = millis();
        mbedtls_sha256_update_ret(&sha, buf, r);
        if (Update.write(buf, r) != static_cast<size_t>(r)) { ok = false; break; }
        done += r;
        int pct = static_cast<int>(static_cast<uint64_t>(done) * 100 / m.size);
        if (pct != lastPct) {
            lastPct = pct;
            setStatus("Installing", pct);
        }
    }
    http.end();
    uint8_t digest[32];
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (!ok || done != m.size || memcmp(digest, m.sha256, sizeof digest) != 0) {
        Update.abort();
        log_w("ota: image rejected (%u/%u bytes, hash %s)", static_cast<unsigned>(done),
              static_cast<unsigned>(m.size), ok ? "mismatch" : "incomplete");
        return setStatus(ok ? "Hash mismatch" : "Download failed");
    }
    if (!Update.end(true)) {
        log_w("ota: Update.end failed: %s", Update.errorString());
        return setStatus("Install failed");
    }
    setStatus("Restarting", 100);
    net::saveNow();  // keep the inbox and pending read/ack events
    log_i("ota: %s installed, restarting", m.version);
    delay(1500);     // let the UI show "Restarting"
    ESP.restart();
}

}  // namespace ota
