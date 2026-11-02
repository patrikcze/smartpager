// Persistent device configuration in NVS (Preferences). See docs/SECURITY.md for the
// protection level: NVS is not encrypted unless flash encryption is enabled.
#pragma once

#include <Arduino.h>

struct Config {
    char ssid[33] = {};
    char wifiPass[65] = {};
    char serverUrl[96] = {};
    char deviceId[41] = {};
    char token[96] = {};
    char name[33] = "SmartPager";
    char tz[64] = "CET-1CEST,M3.5.0,M10.5.0/3";
    uint16_t pollSeconds = 15;
    uint8_t brightness = 70;
    bool longPoll = true;
    bool flip = false;
    bool setupRequested = false;

    bool provisioned() const { return ssid[0] && serverUrl[0] && deviceId[0] && token[0]; }
};

namespace config {
bool load(Config& c);
bool save(const Config& c);
// Root CA (PEM) for HTTPS servers; empty for plain-HTTP development servers.
String loadCaPem();
bool saveCaPem(const String& pem);
// Erases all SmartPager settings (Wi-Fi, server, token). Firmware and the bootloader are untouched.
void factoryReset();
}  // namespace config
