#include "config_store.h"

#include <Preferences.h>

namespace config {
namespace {
constexpr const char* kNs = "smartpager";
constexpr uint8_t kVersion = 1;

void getStr(Preferences& p, const char* key, char* dst, size_t cap) {
    if (p.isKey(key)) p.getString(key, dst, cap);
}
}  // namespace

bool load(Config& c) {
    Preferences p;
    if (!p.begin(kNs, true)) return false;  // namespace does not exist yet
    if (p.getUChar("ver", 0) != kVersion) {
        p.end();
        return false;
    }
    getStr(p, "ssid", c.ssid, sizeof c.ssid);
    getStr(p, "wpass", c.wifiPass, sizeof c.wifiPass);
    getStr(p, "url", c.serverUrl, sizeof c.serverUrl);
    getStr(p, "dev", c.deviceId, sizeof c.deviceId);
    getStr(p, "token", c.token, sizeof c.token);
    getStr(p, "name", c.name, sizeof c.name);
    getStr(p, "tz", c.tz, sizeof c.tz);
    c.pollSeconds = constrain(p.getUShort("poll", c.pollSeconds), 5, 3600);
    c.brightness = constrain(p.getUChar("bright", c.brightness), 5, 100);
    c.longPoll = p.getBool("lpoll", c.longPoll);
    c.flip = p.getBool("flip", c.flip);
    c.setupRequested = p.getBool("setup", false);
    p.end();
    return true;
}

bool save(const Config& c) {
    Preferences p;
    if (!p.begin(kNs, false)) return false;
    p.putUChar("ver", kVersion);
    p.putString("ssid", c.ssid);
    p.putString("wpass", c.wifiPass);
    p.putString("url", c.serverUrl);
    p.putString("dev", c.deviceId);
    p.putString("token", c.token);
    p.putString("name", c.name);
    p.putString("tz", c.tz);
    p.putUShort("poll", c.pollSeconds);
    p.putUChar("bright", c.brightness);
    p.putBool("lpoll", c.longPoll);
    p.putBool("flip", c.flip);
    p.putBool("setup", c.setupRequested);
    p.end();
    return true;
}

String loadCaPem() {
    Preferences p;
    if (!p.begin(kNs, true)) return String();
    String s = p.isKey("ca") ? p.getString("ca") : String();
    p.end();
    return s;
}

bool saveCaPem(const String& pem) {
    Preferences p;
    if (!p.begin(kNs, false)) return false;
    bool ok = pem.length() ? p.putString("ca", pem) == pem.length() : (p.remove("ca"), true);
    p.end();
    return ok;
}

void factoryReset() {
    Preferences p;
    if (p.begin(kNs, false)) {
        p.clear();
        p.end();
    }
}

}  // namespace config
