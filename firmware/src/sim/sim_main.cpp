// Host renderer: runs the real UI code with mock messages and writes PNG screenshots
// at the panel's native 320x170 resolution.   pio run -e sim && .pio/build/sim/program <outdir>
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include <string>
#include <vector>

#include "messaging/message_store.h"
#include "ui/ui.h"

using namespace sp;

static uint16_t fb[kScreenW * kScreenH];
static Message slots[kMaxMessages];
static MessageStore store(slots);
static DeviceStatus status;
static uint32_t fakeNow = 0;
static uint32_t rndState = 7;

static void flush(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* px) {
    for (int y = a->y1; y <= a->y2; ++y)
        for (int x = a->x1; x <= a->x2; ++x) fb[y * kScreenW + x] = (px++)->full;
    lv_disp_flush_ready(d);
}

static void pump(int ms) {
    for (int t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

static void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (i * 8)));
}

static void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> c;
    put32(c, static_cast<uint32_t>(data.size()));
    c.insert(c.end(), type, type + 4);
    c.insert(c.end(), data.begin(), data.end());
    uint32_t crc = static_cast<uint32_t>(::crc32(0, c.data() + 4, static_cast<uInt>(c.size() - 4)));
    put32(c, crc);
    fwrite(c.data(), 1, c.size(), f);
}

// Writes the framebuffer upscaled 2x so details are visible on a desktop monitor.
static void writePng(const std::string& path, int scale = 2) {
    int w = kScreenW * scale, h = kScreenH * scale;
    std::vector<uint8_t> raw;
    raw.reserve((w * 3 + 1) * h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            uint16_t p = fb[(y / scale) * kScreenW + x / scale];
            raw.push_back(static_cast<uint8_t>(((p >> 11) & 0x1F) * 255 / 31));
            raw.push_back(static_cast<uint8_t>(((p >> 5) & 0x3F) * 255 / 63));
            raw.push_back(static_cast<uint8_t>((p & 0x1F) * 255 / 31));
        }
    }
    uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> z(zlen);
    compress2(z.data(), &zlen, raw.data(), static_cast<uLong>(raw.size()), 9);
    z.resize(zlen);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); exit(1); }
    static const uint8_t sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(sig, 1, sizeof sig, f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, w);
    put32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(f, "IHDR", ihdr);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    fclose(f);
    printf("wrote %s\n", path.c_str());
}

static void add(uint32_t id, const char* src, const char* from, const char* subj, const char* body,
                uint8_t pri, uint32_t ago, bool read, bool incremental = false) {
    SyncItem it;
    it.id = id;
    it.seq = id;
    it.pri = pri;
    it.ts = fakeNow - ago;
    it.read = read ? fakeNow - ago / 2 : 0;
    copyUtf8(it.src, sizeof it.src, src);
    copyUtf8(it.sender, sizeof it.sender, from);
    copyUtf8(it.subject, sizeof it.subject, subj);
    copyUtf8(it.body, sizeof it.body, body);
    store.apply(it, incremental);
}

static uint32_t simNow() { return fakeNow; }
static void simLocal(uint32_t t, struct tm* out) {
    time_t tt = t;
    localtime_r(&tt, out);
}
static uint32_t simRand() { return rndState = rndState * 1103515245u + 12345u; }
static void simRead(uint32_t id) { store.markRead(id, fakeNow, simRand); }
static void simAck(uint32_t id) { store.markAck(id, fakeNow, simRand); }

int main(int argc, char** argv) {
    std::string out = argc > 1 ? argv[1] : ".";
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    fakeNow = 1793626500;  // Monday 2 November 2026, 14:35 CET (13:35 UTC)

    lv_init();
    static lv_disp_draw_buf_t buf;
    static lv_color_t px[kScreenW * 40];
    lv_disp_draw_buf_init(&buf, px, nullptr, kScreenW * 40);
    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.hor_res = kScreenW;
    drv.ver_res = kScreenH;
    drv.flush_cb = flush;
    drv.draw_buf = &buf;
    lv_disp_drv_register(&drv);

    status.wifiConnected = true;
    status.rssi = -58;
    strcpy(status.ssid, "HomeNet");
    strcpy(status.ip, "192.168.1.42");
    status.server = Link::Online;
    strcpy(status.serverUrl, "http://192.168.1.10:8080");
    status.lastSync = fakeNow - 4;
    status.timeValid = true;
    status.batteryPct = 101;
    status.brightness = 80;
    status.pollSeconds = 15;
    strcpy(status.deviceId, "pager-1");
    strcpy(status.firmware, "0.1.0");
    status.uptimeS = 3 * 3600 + 120;
    status.freeHeap = 182 * 1024;
    status.freePsram = 7900 * 1024;

    UiContext ctx{};
    ctx.store = &store;
    ctx.status = &status;
    ctx.now = simNow;
    ctx.localTime = simLocal;
    ctx.onRead = simRead;
    ctx.onAck = simAck;

    // 1. empty state
    uiInit(ctx);
    pump(100);
    writePng(out + "/01_home_empty.png");

    // 2. populated
    add(1, "system", "Backup", "Backup completed", "Nightly NAS backup finished in 14 min. 2.4 GB transferred.", 0, 5 * 3600, true);
    add(2, "email", "Jana Nováková", "Příliš žluťoučký kůň", "Ahoj, posílám podklady k zítřejší schůzce. Úterý v 10:00 v zasedačce B.", 1, 3 * 3600, true);
    add(3, "monitoring", "Grafana", "CPU high on node-3", "node-3 CPU at 97% for 10 minutes.\nLoad average 14.2 / 11.8 / 9.1\nTop process: postgres (pid 4121)", 3, 26 * 60, false);
    add(4, "imessage", "Tomáš", "", "See you at 18:00 at the usual place? I'll bring the tickets", 1, 9 * 60, false);
    add(5, "email", "GitHub", "New deployment to production",
        "Deployment #482 of smartpager/main to production succeeded.\n\nCommit: 3f9c2e1 Fix cursor reset after purge\nAuthor: Patrik\nDuration: 2m 14s\n\nChecks: 47 passed, 0 failed. Rollout reached 100% of instances at 14:33 CET. No error-rate regression detected in the first 5 minutes after the rollout.",
        2, 40, false, true);
    pump(50);
    uiTick();
    pump(100);
    writePng(out + "/02_home.png");

    uiShow(Screen::Inbox, 0);
    pump(400);
    writePng(out + "/03_inbox.png");
    uiKey(Key::Next);
    pump(400);
    writePng(out + "/04_inbox_focus2.png");

    uiShow(Screen::Inbox, 0);
    uiKey(Key::Select);  // opens detail, marks read
    pump(400);
    uiTick();
    pump(50);
    writePng(out + "/05_detail.png");
    uiKey(Key::Next);
    pump(400);
    writePng(out + "/06_detail_scrolled.png");
    uiKey(Key::Select);  // acknowledge
    uiTick();
    pump(50);
    writePng(out + "/07_detail_acked.png");

    uiShow(Screen::Settings, 0);
    pump(300);
    writePng(out + "/08_settings.png");
    uiKey(Key::Next);
    uiKey(Key::Next);
    uiKey(Key::Next);
    uiKey(Key::Next);
    pump(400);
    writePng(out + "/09_settings_brightness.png");

    uiShow(Screen::Home);
    add(6, "monitoring", "Uptime Kuma", "api.example.cz is DOWN", "HTTP 502 from 3 regions", 3, 0, false, true);
    uiTick();
    uiNotify(6, 1);
    pump(300);
    writePng(out + "/10_new_message_toast.png");

    status.wifiConnected = false;
    status.server = Link::Offline;
    pump(5000);  // let the toast leave
    fakeNow += 1;
    uiTick();
    pump(100);
    writePng(out + "/11_home_offline.png");
    printf("unread=%u pending_events=%zu\n", store.unreadCount(), store.pendingCount());
    return 0;
}
