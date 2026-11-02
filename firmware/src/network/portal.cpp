#include "portal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <lvgl.h>

#include "app_state.h"
#include "storage/cache_store.h"

namespace portal {
namespace {

WebServer server(80);
DNSServer dns;
char apName[24];
char apPass[12];
String scanOptions;

String esc(const char* s) {
    String o;
    for (; *s; ++s) {
        switch (*s) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#39;"; break;
            default: o += *s;
        }
    }
    return o;
}

const char kHead[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>SmartPager setup</title><style>
:root{color-scheme:dark}body{margin:0;background:#0a0b10;color:#f3f4f8;font:15px/1.45 system-ui,-apple-system,sans-serif}
main{max-width:460px;margin:0 auto;padding:20px 16px 40px}h1{font-size:22px;margin:4px 0 2px}
p.s{color:#7a8194;margin:0 0 18px}fieldset{border:0;background:#1a1c25;border-radius:14px;padding:14px;margin:0 0 14px}
legend{color:#9b8cff;font-weight:600;padding:0;margin-bottom:6px}label{display:block;color:#b4bacb;margin:10px 0 4px;font-size:13px}
input,select,textarea{width:100%;box-sizing:border-box;background:#0f1117;color:#f3f4f8;border:1px solid #2c3040;border-radius:10px;padding:10px;font:inherit}
textarea{min-height:90px;font:12px ui-monospace,monospace}small{color:#7a8194}
button{width:100%;padding:12px;border:0;border-radius:12px;background:#9b8cff;color:#14112b;font-weight:700;font-size:16px}
button.d{background:#2c3040;color:#ff4d5e;margin-top:10px}</style></head><body><main>)HTML";

void handleRoot() {
    const Config& c = g_config;
    String h;
    h.reserve(6000);
    h += FPSTR(kHead);
    h += F("<h1>SmartPager setup</h1><p class=s>Leave secret fields empty to keep the stored value.</p>"
           "<form method=post action=/save>"
           "<fieldset><legend>Wi-Fi</legend><label>Network</label><input name=ssid list=nets required maxlength=32 value=\"");
    h += esc(c.ssid);
    h += F("\"><datalist id=nets>");
    h += scanOptions;
    h += F("</datalist><label>Password</label><input name=wpass type=password maxlength=64 autocomplete=off"
           " placeholder=\"");
    h += c.wifiPass[0] ? F("(stored)") : F("");
    h += F("\"></fieldset><fieldset><legend>Server</legend>"
           "<label>Server URL</label><input name=url required maxlength=95 placeholder=\"https://pager.example.com\" value=\"");
    h += esc(c.serverUrl);
    h += F("\"><small>http:// only on a trusted LAN. https:// works with Let's Encrypt out of the box.</small>"
           "<label>Device ID</label><input name=dev required maxlength=40 pattern=\"[a-z0-9][a-z0-9-]+\" value=\"");
    h += esc(c.deviceId);
    h += F("\"><label>Device token</label><input name=token type=password maxlength=95 autocomplete=off placeholder=\"");
    h += c.token[0] ? F("(stored)") : F("spd_…");
    h += F("\"><label>Custom root CA (PEM) — only for a private CA; leave empty for Let's Encrypt</label><textarea name=ca placeholder=\"-----BEGIN CERTIFICATE-----\">");
    h += esc(config::loadCaPem().c_str());
    h += F("</textarea></fieldset><fieldset><legend>Device</legend><label>Name</label><input name=name maxlength=32 value=\"");
    h += esc(c.name);
    h += F("\"><label>Time zone (POSIX TZ)</label><input name=tz maxlength=63 value=\"");
    h += esc(c.tz);
    h += F("\"><small>e.g. CET-1CEST,M3.5.0,M10.5.0/3 · GMT0BST,M3.5.0/1,M10.5.0 · EST5EDT,M3.2.0,M11.1.0</small>"
           "<label>Polling interval (s)</label><input name=poll type=number min=5 max=3600 value=");
    h += c.pollSeconds;
    h += F("><label>Brightness (%)</label><input name=bright type=number min=5 max=100 value=");
    h += c.brightness;
    h += F("><label><input name=lpoll type=checkbox style=width:auto ");
    h += c.longPoll ? "checked" : "";
    h += F("> Instant delivery (long polling)</label><label><input name=flip type=checkbox style=width:auto ");
    h += c.flip ? "checked" : "";
    h += F("> Rotate screen 180° (USB on the right)</label></fieldset><button>Save and restart</button></form>"
           "<form method=post action=/reset onsubmit=\"return confirm('Erase Wi-Fi, server and token settings?')\">"
           "<button class=d>Factory reset settings</button></form></main></body></html>");
    server.send(200, "text/html; charset=utf-8", h);
}

void copyArg(const char* name, char* dst, size_t cap, bool keepIfEmpty) {
    if (!server.hasArg(name)) return;
    String v = server.arg(name);
    v.trim();
    if (v.isEmpty() && keepIfEmpty) return;
    strlcpy(dst, v.c_str(), cap);
}

void handleSave() {
    Config c = g_config;
    copyArg("ssid", c.ssid, sizeof c.ssid, false);
    copyArg("wpass", c.wifiPass, sizeof c.wifiPass, true);
    copyArg("url", c.serverUrl, sizeof c.serverUrl, false);
    copyArg("dev", c.deviceId, sizeof c.deviceId, false);
    copyArg("token", c.token, sizeof c.token, true);
    copyArg("name", c.name, sizeof c.name, false);
    copyArg("tz", c.tz, sizeof c.tz, false);
    size_t n = strlen(c.serverUrl);
    while (n && c.serverUrl[n - 1] == '/') c.serverUrl[--n] = 0;
    c.pollSeconds = constrain(server.arg("poll").toInt(), 5, 3600);
    c.brightness = constrain(server.arg("bright").toInt(), 5, 100);
    c.longPoll = server.hasArg("lpoll");
    c.flip = server.hasArg("flip");
    c.setupRequested = false;
    String ca = server.arg("ca");
    ca.trim();
    const char* err = nullptr;
    if (strncmp(c.serverUrl, "http://", 7) != 0 && strncmp(c.serverUrl, "https://", 8) != 0) err = "Server URL must start with http:// or https://";
    else if (ca.length() && !ca.startsWith("-----BEGIN CERTIFICATE-----")) err = "The CA certificate must be PEM (-----BEGIN CERTIFICATE-----).";
    else if (ca.length() > 3800) err = "Certificate too large.";
    else if (!c.ssid[0] || !c.deviceId[0] || !c.token[0]) err = "Wi-Fi network, device ID and token are required.";
    if (err) {
        server.send(400, "text/html; charset=utf-8", String(FPSTR(kHead)) + "<h1>Not saved</h1><p>" + err +
                                                          "</p><p><a href=/ style=color:#9b8cff>Back</a></p>");
        return;
    }
    // Server or device changed: the cached inbox belongs to another account. Start fresh.
    if (strcmp(c.serverUrl, g_config.serverUrl) != 0 || strcmp(c.deviceId, g_config.deviceId) != 0) cache::erase();
    config::save(c);
    config::saveCaPem(ca);
    server.send(200, "text/html; charset=utf-8",
                String(FPSTR(kHead)) + "<h1>Saved</h1><p>SmartPager is restarting and will join " + esc(c.ssid) + ".</p>");
    delay(800);
    ESP.restart();
}

void handleReset() {
    config::factoryReset();
    cache::erase();
    server.send(200, "text/html; charset=utf-8", String(FPSTR(kHead)) + "<h1>Settings erased</h1><p>Restarting…</p>");
    delay(800);
    ESP.restart();
}

// USB-serial provisioning (tools/provision.py): one line `provision {json}` with any of
// ssid, wpass, url, dev, token, tz, name. Physical USB access can reflash the device anyway,
// so this grants nothing new. Values are saved; setup continues so Wi-Fi can still be set
// in the browser, unless `"restart": true` is sent.
void pollSerial() {
    static String line;
    while (Serial.available()) {
        char ch = static_cast<char>(Serial.read());
        if (ch == '\r') continue;
        if (ch != '\n') {
            if (line.length() < 1024) line += ch;
            continue;
        }
        if (line.startsWith("provision ")) {
            JsonDocument doc;
            if (deserializeJson(doc, line.c_str() + 10)) {
                Serial.println("[portal] provision: invalid JSON");
            } else {
                Config c = g_config;
                auto set = [&](const char* k, char* dst, size_t cap) {
                    if (doc[k].is<const char*>()) strlcpy(dst, doc[k].as<const char*>(), cap);
                };
                set("ssid", c.ssid, sizeof c.ssid);
                set("wpass", c.wifiPass, sizeof c.wifiPass);
                set("url", c.serverUrl, sizeof c.serverUrl);
                set("dev", c.deviceId, sizeof c.deviceId);
                set("token", c.token, sizeof c.token);
                set("tz", c.tz, sizeof c.tz);
                set("name", c.name, sizeof c.name);
                if (strcmp(c.serverUrl, g_config.serverUrl) != 0 || strcmp(c.deviceId, g_config.deviceId) != 0)
                    cache::erase();
                g_config = c;
                config::save(c);
                Serial.printf("[portal] provision: saved (wifi %s, server %s, device %s, token %s)\n",
                              c.ssid[0] ? "set" : "missing", c.serverUrl[0] ? "set" : "missing",
                              c.deviceId[0] ? "set" : "missing", c.token[0] ? "set" : "missing");
                if ((doc["restart"] | false) && c.provisioned()) {
                    delay(200);
                    ESP.restart();
                }
            }
        }
        line = "";
    }
}

void showScreen() {
    lv_obj_t* s = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x0A0B10), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    auto add = [&](const lv_font_t* f, uint32_t color, int y, const String& t) {
        lv_obj_t* l = lv_label_create(s);
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
        lv_label_set_text(l, t.c_str());
        lv_obj_set_pos(l, 14, y);
    };
    add(&sp_font_16, 0x9B8CFF, 8, LV_SYMBOL_SETTINGS "  Setup mode");
    add(&sp_font_12, 0x7A8194, 34, "1. Join this Wi-Fi network");
    add(&sp_font_16, 0xF3F4F8, 50, apName);
    add(&sp_font_12, 0x7A8194, 76, "Password");
    add(&sp_font_22, 0xF3F4F8, 90, apPass);
    add(&sp_font_12, 0x7A8194, 124, "2. Open in a browser");
    add(&sp_font_16, 0xF3F4F8, 140, "http://192.168.4.1");
    lv_scr_load(s);
}

}  // namespace

void run() {
    esp_task_wdt_delete(nullptr);  // waiting for the user can take arbitrarily long
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(apName, sizeof apName, "SmartPager-%02X%02X", mac[4], mac[5]);
    // Fresh random password each time setup mode starts; no shared default credentials.
    static const char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";  // no look-alike characters
    for (size_t i = 0; i < 10; ++i) apPass[i] = kAlphabet[esp_random() % (sizeof kAlphabet - 1)];
    apPass[10] = 0;

    WiFi.mode(WIFI_AP_STA);
    int found = WiFi.scanNetworks();
    for (int i = 0; i < found && i < 20; ++i) scanOptions += "<option value=\"" + esc(WiFi.SSID(i).c_str()) + "\">";
    WiFi.scanDelete();
    WiFi.softAP(apName, apPass);
    dns.start(53, "*", WiFi.softAPIP());
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/reset", HTTP_POST, handleReset);
    server.onNotFound([] {  // captive-portal style redirect
        server.sendHeader("Location", "http://192.168.4.1/", true);
        server.send(302, "text/plain", "");
    });
    server.begin();
    showScreen();
    log_i("setup portal running");
    Serial.println("[portal] ready");
    for (;;) {
        pollSerial();
        dns.processNextRequest();
        server.handleClient();
        lv_timer_handler();
        delay(5);
    }
}

}  // namespace portal
