// SmartPager firmware entry point.
//   core 1: Arduino loop = UI task (LVGL, buttons, touch, backlight)
//   core 0: network task (Wi-Fi, NTP, sync, event upload, cache persistence)
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <lvgl.h>
#include <time.h>

#include "app_state.h"
#include "hardware/board.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "network/portal.h"
#include "network/sync_client.h"
#include "storage/cache_store.h"
#include "ui/ui.h"

// ---------------------------------------------------------------- shared state
// Allocated in setup(): PSRAM is not initialized yet while global constructors run.
sp::MessageStore g_store(nullptr);
sp::DeviceStatus g_status;
Config g_config;
SemaphoreHandle_t g_lock = nullptr;
QueueHandle_t g_notifyQueue = nullptr;

namespace {

constexpr uint32_t kDimAfterMs = 60 * 1000;
constexpr uint8_t kDimPercent = 8;
constexpr uint32_t kWatchdogS = 45;  // > longest blocking HTTP call (long poll 20 s + 10 s)

sp::DeviceStatus statusShadow;  // UI-owned copy, refreshed under the lock every loop
bool dimmed = false;
uint32_t lastBatteryMs = 0;
int batteryPct = -1;

uint32_t nowUnix() {
    time_t t = time(nullptr);
    return t > 1700000000 ? static_cast<uint32_t>(t) : 0;
}

void toLocal(uint32_t t, struct tm* out) {
    time_t tt = t;
    localtime_r(&tt, out);
}

uint32_t randomWord() { return esp_random(); }

void onRead(uint32_t id) {
    appLock();
    bool changed = g_store.markRead(id, nowUnix(), randomWord);
    appUnlock();
    if (changed) {
        net::requestSave();
        net::kick();
    }
}

void onAck(uint32_t id) {
    appLock();
    bool changed = g_store.markAck(id, nowUnix(), randomWord);
    appUnlock();
    if (changed) {
        net::requestSave();
        net::kick();
    }
}

void saveConfig() { config::save(g_config); }

void onSetting(sp::SettingAction a) {
    switch (a) {
        case sp::SettingAction::Brightness: {
            static const uint8_t steps[] = {20, 40, 60, 80, 100};
            uint8_t next = steps[0];
            for (uint8_t s : steps)
                if (s > g_config.brightness) { next = s; break; }
            g_config.brightness = next;
            display::setBrightness(next);
            saveConfig();
            break;
        }
        case sp::SettingAction::PollInterval: {
            // push (long poll) -> 5 s -> 15 s -> 30 s -> 60 s -> push
            static const uint16_t steps[] = {5, 15, 30, 60};
            if (g_config.longPoll) {
                g_config.longPoll = false;
                g_config.pollSeconds = steps[0];
            } else {
                size_t i = 0;
                while (i < 4 && steps[i] <= g_config.pollSeconds) ++i;
                if (i == 4) {
                    g_config.longPoll = true;
                    g_config.pollSeconds = 15;
                } else {
                    g_config.pollSeconds = steps[i];
                }
            }
            saveConfig();
            net::kick();
            break;
        }
        case sp::SettingAction::SyncNow:
            net::kick();
            break;
        case sp::SettingAction::SetupMode:
            g_config.setupRequested = true;
            saveConfig();
            delay(100);
            ESP.restart();
    }
}

// The ADC sees half the battery voltage. On USB power the charger holds it above ~4.3 V,
// so the percentage is only meaningful on battery; it is an approximation (no fuel gauge).
int readBattery() {
    uint32_t mv = analogReadMilliVolts(board::kBatteryAdc) * 2;
    if (mv > 4350 || mv < 2500) return 101;  // USB / charging, or no battery connected
    int pct = static_cast<int>((static_cast<int32_t>(mv) - 3300) * 100 / (4150 - 3300));
    return constrain(pct, 0, 100);
}

void wake() {
    lv_disp_trig_activity(nullptr);
    if (dimmed) {
        dimmed = false;
        display::setBrightness(g_config.brightness);
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    log_i("SmartPager " SMARTPAGER_VERSION);
    bool haveConfig = config::load(g_config);
    if (!display::begin(g_config.flip)) {
        log_e("display init failed");
    }
    input::begin(g_config.flip);
    display::setBrightness(g_config.brightness);
    cache::begin();  // mounted before setup mode so the portal can clear a stale inbox

    // Setup mode: not provisioned, requested from Settings, or KEY (GPIO14) held at power-up.
    // GPIO0 is deliberately not used here: holding it at reset enters the ROM bootloader.
    if (!haveConfig || !g_config.provisioned() || g_config.setupRequested || input::keyHeldRaw()) {
        if (g_config.setupRequested) {
            g_config.setupRequested = false;
            config::save(g_config);
        }
        portal::run();
    }

    auto* slots = static_cast<sp::Message*>(ps_calloc(sp::kMaxMessages, sizeof(sp::Message)));
    g_lock = xSemaphoreCreateMutex();
    g_notifyQueue = xQueueCreate(4, sizeof(NotifyMsg));
    if (!slots || !g_lock || !g_notifyQueue) {
        log_e("out of memory at boot");
        delay(2000);
        ESP.restart();
    }
    g_store = sp::MessageStore(slots);
    bool cached = cache::load(g_store);
    // Boot summary on the console (counts only, never message contents).
    Serial.printf("[boot] SmartPager %s  psram=%u KB  touch=%s  cache=%s messages=%u pending=%u cursor=%u\n",
                  SMARTPAGER_VERSION, static_cast<unsigned>(ESP.getPsramSize() >> 10),
                  input::touchPresent() ? "cst816" : "none", cached ? "loaded" : "empty",
                  static_cast<unsigned>(g_store.count()), static_cast<unsigned>(g_store.pendingCount()),
                  static_cast<unsigned>(g_store.cursor()));

    strlcpy(g_status.ssid, g_config.ssid, sizeof g_status.ssid);
    strlcpy(g_status.serverUrl, g_config.serverUrl, sizeof g_status.serverUrl);
    strlcpy(g_status.deviceId, g_config.deviceId, sizeof g_status.deviceId);
    strlcpy(g_status.deviceName, g_config.name, sizeof g_status.deviceName);
    strlcpy(g_status.firmware, SMARTPAGER_VERSION, sizeof g_status.firmware);
    statusShadow = g_status;

    sp::UiContext ctx{};
    ctx.store = &g_store;
    ctx.status = &statusShadow;
    ctx.lock = appLock;
    ctx.unlock = appUnlock;
    ctx.now = nowUnix;
    ctx.localTime = toLocal;
    ctx.onRead = onRead;
    ctx.onAck = onAck;
    ctx.onSetting = onSetting;
    sp::uiInit(ctx);

    esp_task_wdt_init(kWatchdogS, true);  // reconfigures the TWDT started by the core
    esp_task_wdt_add(nullptr);
    net::start();
}

void loop() {
    esp_task_wdt_reset();
    lv_timer_handler();

    sp::Key key;
    if (input::poll(key)) {
        bool wasDimmed = dimmed;
        wake();
        if (!wasDimmed) sp::uiKey(key);  // the first press only wakes the screen
    }
    if (!dimmed && lv_disp_get_inactive_time(nullptr) > kDimAfterMs) {
        dimmed = true;
        display::setBrightness(kDimPercent < g_config.brightness ? kDimPercent : g_config.brightness);
    } else if (dimmed && lv_disp_get_inactive_time(nullptr) < 100) {
        wake();  // touch activity
    }

    NotifyMsg n;
    if (xQueueReceive(g_notifyQueue, &n, 0) == pdTRUE) {
        sp::uiTick();  // make sure the new rows exist before the banner references them
        sp::uiNotify(n.messageId, n.count);
        wake();
    }

    uint32_t now = millis();
    if (now - lastBatteryMs > 5000 || lastBatteryMs == 0) {
        lastBatteryMs = now;
        batteryPct = readBattery();
    }
    appLock();
    statusShadow = g_status;
    appUnlock();
    statusShadow.batteryPct = batteryPct;
    statusShadow.brightness = g_config.brightness;
    statusShadow.pollSeconds = g_config.pollSeconds;
    statusShadow.longPoll = g_config.longPoll;
    statusShadow.touch = input::touchPresent();
    statusShadow.uptimeS = now / 1000;
    statusShadow.freeHeap = ESP.getFreeHeap();
    statusShadow.freePsram = ESP.getFreePsram();
    sp::uiTick();
    delay(5);
}
