// SmartPager LVGL user interface for a 320x170 landscape panel.
// Platform-independent: the device and the host renderer (src/sim) provide UiContext.
#pragma once

#include <stdint.h>
#include <time.h>

#include "../messaging/message_store.h"

namespace sp {

constexpr int kScreenW = 320;
constexpr int kScreenH = 170;

enum class Key : uint8_t { Prev, Next, Select, Back };

enum class Link : uint8_t { Offline, Connecting, Online, AuthError, Error };

struct DeviceStatus {
    bool wifiConnected = false;
    int8_t rssi = 0;
    char ssid[33] = {};
    char ip[16] = {};
    Link server = Link::Offline;
    char serverUrl[96] = {};
    char lastError[48] = {};
    uint32_t lastSync = 0;      // unix seconds of last successful sync
    bool timeValid = false;
    int batteryPct = -1;        // -1 unknown, 101 = on USB power
    uint8_t brightness = 80;    // percent
    uint16_t pollSeconds = 15;
    bool longPoll = true;
    char deviceId[41] = {};
    char deviceName[33] = {};
    char firmware[16] = {};
    bool touch = false;
    uint32_t uptimeS = 0;
    uint32_t freeHeap = 0;
    uint32_t freePsram = 0;
};

enum class SettingAction : uint8_t { Brightness, PollInterval, SyncNow, SetupMode };

struct UiContext {
    MessageStore* store;
    const DeviceStatus* status;
    void (*lock)();
    void (*unlock)();
    uint32_t (*now)();                    // unix seconds (0 if clock not set)
    void (*localTime)(uint32_t, struct tm*);
    void (*onRead)(uint32_t id);          // user opened a message
    void (*onAck)(uint32_t id);           // user acknowledged a message
    void (*onSetting)(SettingAction a);
};

void uiInit(const UiContext& ctx);
void uiKey(Key k);
// Call every ~20-50 ms from the UI task after lv_timer_handler(); refreshes only what changed.
void uiTick();
// Shows the "new message" banner for the given message id.
void uiNotify(uint32_t messageId, uint16_t newCount);

// For the host renderer: jump straight to a screen.
enum class Screen : uint8_t { Home, Inbox, Detail, Settings };
void uiShow(Screen s, int focus = 0);

}  // namespace sp
