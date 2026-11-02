#include "ui.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "format.h"

namespace sp {

// ------------------------------------------------------------------ palette

namespace col {
constexpr uint32_t bg = 0x0A0B10;
constexpr uint32_t card = 0x1A1C25;
constexpr uint32_t cardHi = 0x272A37;
constexpr uint32_t line = 0x2C3040;
constexpr uint32_t text = 0xF3F4F8;
constexpr uint32_t sub = 0xB4BACB;
constexpr uint32_t dim = 0x7A8194;
constexpr uint32_t accent = 0x9B8CFF;  // soft lavender
constexpr uint32_t ok = 0x34C77B;
constexpr uint32_t warn = 0xFFB020;
constexpr uint32_t bad = 0xFF4D5E;
}  // namespace col

static inline lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

// ------------------------------------------------------------------ state

static UiContext ctx;
static DeviceStatus shownStatus;
static Screen current = Screen::Home;
static lv_obj_t* screens[4];
static uint32_t shownVersion = 0;
static int shownMinute = -1;
static uint32_t lastStatusRefresh = 0;

static lv_style_t stCard, stRowFocus, stChip;

struct StatusIcons {
    lv_obj_t* wifi;
    lv_obj_t* server;
    lv_obj_t* battery;
};
static StatusIcons icons[4];

// home
static lv_obj_t *hTime, *hDate, *hPill, *hPillLabel, *hFooter, *hCards, *hMore;
// inbox
static lv_obj_t *iList, *iCount, *iEmpty;
static uint32_t iIds[kMaxMessages];
static lv_obj_t* iRows[kMaxMessages];
static size_t iRowCount = 0;
static int iFocus = 0;
// detail
static lv_obj_t *dBody, *dHeaderIcon, *dHeaderSrc, *dPri, *dSender, *dStamp, *dSubject, *dText, *dFooter;
static uint32_t dId = 0;
// settings
enum SettingRow {
    kRowWifi, kRowIp, kRowServer, kRowSync, kRowBrightness, kRowPoll, kRowSyncNow,
    kRowDevice, kRowTouch, kRowMemory, kRowUptime, kRowFirmware, kRowSetup, kRowCount
};
static const char* const kRowLabels[kRowCount] = {
    "Wi-Fi", "IP address", "Server", "Last sync", "Brightness", "Polling", "Sync now",
    "Device", "Touch", "Memory", "Uptime", "Firmware", "Setup mode"};
static lv_obj_t *sList, *sRows[kRowCount], *sValues[kRowCount];
static int sFocus = 0;
// toast
static lv_obj_t *tBox, *tIcon, *tIconBg, *tTitle, *tText;

// ------------------------------------------------------------------ helpers

static lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* text = "") {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, C(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t* fixedLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color, int x, int y, int w) {
    lv_obj_t* l = label(parent, font, color);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_obj_set_height(l, lv_font_get_line_height(font));  // one line; DOT mode adds "..."
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

// Round tinted badge with a source icon, like a wearable notification avatar.
static lv_obj_t* sourceBadge(lv_obj_t* parent, int x, int y, int size, const SourceStyle& s,
                             const lv_font_t* font) {
    lv_obj_t* b = box(parent, x, y, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, C(s.color), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
    lv_obj_t* i = label(b, font, s.color, s.icon);
    lv_obj_center(i);
    return b;
}

static lv_obj_t* newScreen() {
    lv_obj_t* s = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, kScreenW, kScreenH);
    lv_obj_set_style_bg_color(s, C(col::bg), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s, C(col::text), 0);
    lv_obj_set_style_text_font(s, &sp_font_14, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

static void lockStore() { if (ctx.lock) ctx.lock(); }
static void unlockStore() { if (ctx.unlock) ctx.unlock(); }

static void localTm(uint32_t t, struct tm* out) {
    memset(out, 0, sizeof *out);
    if (ctx.localTime) ctx.localTime(t, out);
}

static void ageText(char* buf, size_t cap, uint32_t ts) {
    uint32_t now = ctx.now ? ctx.now() : 0;
    if (!now) { buf[0] = 0; return; }
    struct tm nl, tl;
    localTm(now, &nl);
    localTm(ts, &tl);
    formatAge(buf, cap, now, ts, nl, tl);
}

// ------------------------------------------------------------------ status icons

static StatusIcons makeStatusIcons(lv_obj_t* parent, int x, int y) {
    lv_obj_t* row = box(parent, x, y, 74, 16);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    StatusIcons s;
    s.server = label(row, &sp_font_12, col::dim, "\xE2\x80\xA2");
    s.wifi = label(row, &sp_font_12, col::dim, LV_SYMBOL_WIFI);
    s.battery = label(row, &sp_font_12, col::dim, "");
    return s;
}

static uint32_t linkColor(Link l) {
    switch (l) {
        case Link::Online: return col::ok;
        case Link::Connecting: return col::warn;
        case Link::AuthError:
        case Link::Error: return col::bad;
        default: return col::dim;
    }
}

static void updateStatusIcons(const DeviceStatus& st) {
    char bat[24];
    if (st.batteryPct > 100) snprintf(bat, sizeof bat, LV_SYMBOL_CHARGE);
    else if (st.batteryPct < 0) bat[0] = 0;
    else {
        const char* sym = st.batteryPct > 80 ? LV_SYMBOL_BATTERY_FULL : st.batteryPct > 55 ? LV_SYMBOL_BATTERY_3
                          : st.batteryPct > 30 ? LV_SYMBOL_BATTERY_2 : st.batteryPct > 10 ? LV_SYMBOL_BATTERY_1
                          : LV_SYMBOL_BATTERY_EMPTY;
        snprintf(bat, sizeof bat, "%s", sym);
    }
    uint32_t wifiColor = !st.wifiConnected ? col::bad : st.rssi > -67 ? col::text : st.rssi > -78 ? col::sub : col::warn;
    for (StatusIcons& s : icons) {
        if (!s.wifi) continue;
        lv_obj_set_style_text_color(s.wifi, C(wifiColor), 0);
        // server link state is shown as a coloured dot
        lv_label_set_text(s.server, "\xE2\x80\xA2");
        lv_obj_set_style_text_color(s.server, C(linkColor(st.server)), 0);
        lv_label_set_text(s.battery, bat);
        lv_obj_set_style_text_color(s.battery, C(st.batteryPct >= 0 && st.batteryPct <= 10 ? col::bad : col::sub), 0);
    }
}

// ------------------------------------------------------------------ toast

static void toastY(void* obj, int32_t v) { lv_obj_set_y(static_cast<lv_obj_t*>(obj), v); }

static void buildToast() {
    tBox = box(lv_layer_top(), 8, -64, kScreenW - 16, 54);
    lv_obj_add_style(tBox, &stCard, 0);
    lv_obj_set_style_bg_color(tBox, C(col::cardHi), 0);
    lv_obj_set_style_border_width(tBox, 1, 0);
    lv_obj_set_style_border_color(tBox, C(col::accent), 0);
    tIconBg = box(tBox, 10, 11, 32, 32);
    lv_obj_set_style_radius(tIconBg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(tIconBg, LV_OPA_20, 0);
    tIcon = label(tIconBg, &sp_font_16, col::accent, "");
    lv_obj_center(tIcon);
    tTitle = fixedLabel(tBox, &sp_font_16, col::text, 52, 7, kScreenW - 16 - 62);
    tText = fixedLabel(tBox, &sp_font_14, col::sub, 52, 28, kScreenW - 16 - 62);
}

void uiNotify(uint32_t id, uint16_t newCount) {
    lockStore();
    int idx = ctx.store->indexOf(id);
    if (idx < 0) { unlockStore(); return; }
    const Message& m = ctx.store->at(static_cast<size_t>(idx));
    const SourceStyle& s = sourceStyle(m.src);
    char title[96], text[128];
    if (newCount > 1) snprintf(title, sizeof title, "%u new messages", newCount);
    else snprintf(title, sizeof title, "%s", *m.sender ? m.sender : s.label);
    previewTitle(text, sizeof text, m.subject, m.body);
    uint32_t border = m.pri >= 3 ? col::bad : m.pri == 2 ? col::warn : col::accent;
    unlockStore();

    lv_label_set_text(tIcon, s.icon);
    lv_obj_set_style_text_color(tIcon, C(s.color), 0);
    lv_obj_set_style_bg_color(tIconBg, C(s.color), 0);
    lv_label_set_text(tTitle, title);
    lv_label_set_text(tText, text);
    lv_obj_set_style_border_color(tBox, C(border), 0);

    lv_anim_del(tBox, toastY);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, tBox);
    lv_anim_set_exec_cb(&a, toastY);
    lv_anim_set_values(&a, -64, 8);
    lv_anim_set_time(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_playback_delay(&a, 3800);
    lv_anim_set_playback_time(&a, 220);
    lv_anim_start(&a);
}

// ------------------------------------------------------------------ home

static void buildHome() {
    lv_obj_t* s = screens[0] = newScreen();
    lv_obj_t* brand = label(s, &sp_font_12, col::dim, "SMARTPAGER");
    lv_obj_set_style_text_letter_space(brand, 1, 0);
    lv_obj_set_pos(brand, 12, 7);
    icons[0] = makeStatusIcons(s, 84, 6);

    hTime = label(s, &sp_font_clock, col::text, "--:--");
    lv_obj_set_pos(hTime, 9, 25);
    hDate = fixedLabel(s, &sp_font_14, col::sub, 12, 84, 150);

    hPill = box(s, 12, 112, 120, 26);
    lv_obj_set_style_radius(hPill, 13, 0);
    lv_obj_set_style_bg_opa(hPill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(hPill, 12, 0);
    lv_obj_set_width(hPill, LV_SIZE_CONTENT);
    hPillLabel = label(hPill, &sp_font_14, col::text, "");
    lv_obj_center(hPillLabel);

    hFooter = fixedLabel(s, &sp_font_12, col::dim, 12, 150, 150);

    hCards = box(s, 164, 6, 148, 158);
    hMore = label(s, &sp_font_12, col::dim, "");
}

static void homeCard(lv_obj_t* parent, int y, const Message& m) {
    const SourceStyle& st = sourceStyle(m.src);
    lv_obj_t* c = box(parent, 0, y, 148, 64);
    lv_obj_add_style(c, &stCard, 0);
    if (!m.readAt) {
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, C((m.flags & kFresh) ? col::accent : col::line), 0);
    }
    sourceBadge(c, 8, 7, 18, st, &sp_font_12);
    char age[16];
    ageText(age, sizeof age, m.ts);
    lv_obj_t* a = label(c, &sp_font_12, col::dim, age);
    lv_obj_align(a, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_t* who = fixedLabel(c, &sp_font_12, m.readAt ? col::sub : col::text, 31, 8, 80);
    lv_label_set_text(who, *m.sender ? m.sender : st.label);
    char title[kSubjectLen];
    previewTitle(title, sizeof title, m.subject, m.body);
    lv_obj_t* t = fixedLabel(c, &sp_font_14, m.readAt ? col::dim : col::text, 8, 27, 132);
    lv_obj_set_height(t, 34);
    lv_label_set_text(t, title);
}

static void refreshHome() {
    lockStore();
    uint16_t unread = ctx.store->unreadCount();
    size_t n = ctx.store->count();
    lv_obj_clean(hCards);
    size_t shown = 0;
    // prefer unread messages for the preview; fall back to the most recent ones
    for (int pass = 0; pass < 2 && shown < 2; ++pass)
        for (size_t i = 0; i < n && shown < 2; ++i) {
            const Message& m = ctx.store->at(i);
            bool want = pass == 0 ? !m.readAt : m.readAt;
            if (want) homeCard(hCards, static_cast<int>(shown++) * 70, m);
        }
    unlockStore();

    if (n == 0) {
        lv_obj_t* c = box(hCards, 0, 0, 148, 134);
        lv_obj_add_style(c, &stCard, 0);
        lv_obj_t* ring = box(c, 54, 22, 40, 40);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ring, C(col::ok), 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_20, 0);
        lv_obj_center(label(ring, &sp_font_16, col::ok, LV_SYMBOL_OK));
        lv_obj_t* l1 = label(c, &sp_font_16, col::text, "All caught up");
        lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 72);
        lv_obj_t* l2 = label(c, &sp_font_12, col::dim, "No messages yet");
        lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 96);
    }
    if (n > 2) {
        char more[24];
        snprintf(more, sizeof more, "+%u more", static_cast<unsigned>(n - 2));
        lv_label_set_text(hMore, more);
        lv_obj_align(hMore, LV_ALIGN_TOP_LEFT, 164 + 74 - lv_txt_get_width(more, strlen(more), &sp_font_12, 0, 0) / 2, 148);
        lv_obj_clear_flag(hMore, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(hMore, LV_OBJ_FLAG_HIDDEN);
    }

    char pill[32];
    if (unread) {
        snprintf(pill, sizeof pill, LV_SYMBOL_BELL "  %u new", unread);
        lv_obj_set_style_bg_color(hPill, C(col::accent), 0);
        lv_obj_set_style_text_color(hPillLabel, C(0x14112B), 0);
        lv_obj_set_style_text_font(hPillLabel, &sp_font_16, 0);
    } else {
        snprintf(pill, sizeof pill, LV_SYMBOL_OK "  No new messages");
        lv_obj_set_style_bg_color(hPill, C(col::card), 0);
        lv_obj_set_style_text_color(hPillLabel, C(col::sub), 0);
        lv_obj_set_style_text_font(hPillLabel, &sp_font_12, 0);
    }
    lv_label_set_text(hPillLabel, pill);
}

static void refreshClock() {
    uint32_t now = ctx.now ? ctx.now() : 0;
    if (!now) {
        lv_label_set_text(hTime, "--:--");
        lv_label_set_text(hDate, "Waiting for time…");
        return;
    }
    struct tm t;
    localTm(now, &t);
    char buf[40];
    formatClock(buf, sizeof buf, t);
    lv_label_set_text(hTime, buf);
    formatLongDate(buf, sizeof buf, t);
    lv_label_set_text(hDate, buf);
}

static void refreshHomeFooter(const DeviceStatus& st) {
    char buf[64];
    uint32_t now = ctx.now ? ctx.now() : 0;
    if (!st.wifiConnected) snprintf(buf, sizeof buf, "Wi-Fi offline · cached");
    else if (st.server == Link::AuthError) snprintf(buf, sizeof buf, "Server rejected token");
    else if (st.server != Link::Online) snprintf(buf, sizeof buf, "Server offline · retrying");
    else if (st.lastSync && now >= st.lastSync) {
        char d[16];
        if (now - st.lastSync < 60) snprintf(buf, sizeof buf, "Synced just now");
        else { formatDuration(d, sizeof d, now - st.lastSync); snprintf(buf, sizeof buf, "Synced %s ago", d); }
    } else snprintf(buf, sizeof buf, "Online");
    lv_label_set_text(hFooter, buf);
    lv_obj_set_style_text_color(hFooter, C(st.server == Link::Online ? col::dim : col::warn), 0);
}

// ------------------------------------------------------------------ inbox

static void openDetail(uint32_t id);

static void rowClicked(lv_event_t* e) {
    openDetail(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e))));
}

static void buildInbox() {
    lv_obj_t* s = screens[1] = newScreen();
    lv_obj_t* title = label(s, &sp_font_16, col::text, "Inbox");
    lv_obj_set_pos(title, 12, 4);
    iCount = label(s, &sp_font_12, col::accent, "");
    lv_obj_set_pos(iCount, 66, 8);
    icons[1] = makeStatusIcons(s, 238, 6);

    iList = box(s, 0, 26, kScreenW, kScreenH - 26);
    lv_obj_add_flag(iList, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(iList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(iList, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(iList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(iList, 8, 0);
    lv_obj_set_style_pad_top(iList, 2, 0);
    lv_obj_set_style_pad_bottom(iList, 8, 0);
    lv_obj_set_style_pad_row(iList, 6, 0);

    iEmpty = label(s, &sp_font_14, col::dim, LV_SYMBOL_OK "  Inbox is empty");
    lv_obj_align(iEmpty, LV_ALIGN_CENTER, 0, 10);
}

static void inboxRow(const Message& m, size_t idx) {
    const SourceStyle& st = sourceStyle(m.src);
    lv_obj_t* r = box(iList, 0, 0, kScreenW - 16, 50);
    lv_obj_add_style(r, &stCard, 0);
    lv_obj_add_style(r, &stRowFocus, LV_STATE_CHECKED);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, rowClicked, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(m.id)));
    if (!m.readAt) {
        lv_obj_t* bar = box(r, 0, 12, 3, 26);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, C(st.color), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }
    lv_obj_t* badge = sourceBadge(r, 10, 11, 28, st, &sp_font_14);
    if (m.flags & kFresh) {
        lv_obj_t* dot = box(r, 32, 9, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, C(col::accent), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }
    (void)badge;
    char age[16];
    ageText(age, sizeof age, m.ts);
    lv_obj_t* a = label(r, &sp_font_12, col::dim, age);
    lv_obj_align(a, LV_ALIGN_TOP_RIGHT, -10, 7);
    int senderW = 190;
    if (m.pri >= 2) {
        lv_obj_t* p = label(r, &sp_font_12, priorityColor(m.pri), m.pri >= 3 ? LV_SYMBOL_WARNING : "\xE2\x80\xA2");
        lv_obj_align(p, LV_ALIGN_TOP_RIGHT, -46, 7);
        senderW = 170;
    }
    lv_obj_t* who = fixedLabel(r, &sp_font_16, m.readAt ? col::sub : col::text, 48, 5, senderW);
    lv_label_set_text(who, *m.sender ? m.sender : st.label);
    char title[kSubjectLen];
    previewTitle(title, sizeof title, m.subject, m.body);
    lv_obj_t* t = fixedLabel(r, &sp_font_14, m.readAt ? col::dim : col::sub, 48, 27, kScreenW - 16 - 58);
    lv_label_set_text(t, title);
    iRows[idx] = r;
    iIds[idx] = m.id;
}

static void focusInbox(int idx, bool anim) {
    if (iRowCount == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= static_cast<int>(iRowCount)) idx = static_cast<int>(iRowCount) - 1;
    if (iFocus < static_cast<int>(iRowCount)) lv_obj_clear_state(iRows[iFocus], LV_STATE_CHECKED);
    iFocus = idx;
    lv_obj_add_state(iRows[iFocus], LV_STATE_CHECKED);
    lv_obj_scroll_to_view(iRows[iFocus], anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

static void refreshInbox() {
    uint32_t focusedId = (iFocus < static_cast<int>(iRowCount)) ? iIds[iFocus] : 0;
    lv_obj_clean(iList);
    iRowCount = 0;
    lockStore();
    size_t n = ctx.store->count();
    for (size_t i = 0; i < n; ++i) inboxRow(ctx.store->at(i), iRowCount++);
    uint16_t unread = ctx.store->unreadCount();
    unlockStore();
    char buf[24];
    snprintf(buf, sizeof buf, unread ? "%u unread" : "", unread);
    lv_label_set_text(iCount, buf);
    if (iRowCount == 0) lv_obj_clear_flag(iEmpty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(iEmpty, LV_OBJ_FLAG_HIDDEN);
    int idx = 0;
    for (size_t i = 0; i < iRowCount; ++i)
        if (iIds[i] == focusedId) idx = static_cast<int>(i);
    iFocus = 0;
    if (iRowCount) {
        lv_obj_update_layout(iList);
        focusInbox(idx, false);
    }
}

// ------------------------------------------------------------------ detail

static void buildDetail() {
    lv_obj_t* s = screens[2] = newScreen();
    dHeaderIcon = box(s, 10, 5, 20, 20);
    lv_obj_set_style_radius(dHeaderIcon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dHeaderIcon, LV_OPA_20, 0);
    lv_obj_center(label(dHeaderIcon, &sp_font_12, col::text, ""));
    dHeaderSrc = label(s, &sp_font_12, col::sub, "");
    lv_obj_set_pos(dHeaderSrc, 36, 8);
    dPri = label(s, &sp_font_12, col::text, "");
    lv_obj_add_style(dPri, &stChip, 0);

    dBody = box(s, 0, 28, kScreenW, kScreenH - 28 - 22);
    lv_obj_add_flag(dBody, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(dBody, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(dBody, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(dBody, C(col::accent), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(dBody, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(dBody, 3, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(dBody, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(dBody, 12, 0);
    lv_obj_set_style_pad_bottom(dBody, 10, 0);
    lv_obj_set_style_pad_row(dBody, 3, 0);

    dSender = label(dBody, &sp_font_16, col::text, "");
    lv_obj_set_width(dSender, LV_PCT(100));
    lv_label_set_long_mode(dSender, LV_LABEL_LONG_DOT);
    dStamp = label(dBody, &sp_font_12, col::dim, "");
    dSubject = label(dBody, &sp_font_16, col::accent, "");
    lv_obj_set_width(dSubject, LV_PCT(100));
    lv_label_set_long_mode(dSubject, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_top(dSubject, 4, 0);
    dText = label(dBody, &sp_font_14, 0xD5D9E3, "");
    lv_obj_set_width(dText, LV_PCT(100));
    lv_label_set_long_mode(dText, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(dText, 2, 0);

    lv_obj_t* foot = box(s, 0, kScreenH - 22, kScreenW, 22);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(foot, 1, 0);
    lv_obj_set_style_border_color(foot, C(col::line), 0);
    dFooter = label(foot, &sp_font_12, col::dim, "");
    lv_obj_align(dFooter, LV_ALIGN_LEFT_MID, 12, 0);
}

static bool refreshDetail() {
    lockStore();
    int idx = ctx.store->indexOf(dId);
    if (idx < 0) { unlockStore(); return false; }
    const Message& m = ctx.store->at(static_cast<size_t>(idx));
    const SourceStyle& st = sourceStyle(m.src);
    lv_obj_set_style_bg_color(dHeaderIcon, C(st.color), 0);
    lv_obj_t* ic = lv_obj_get_child(dHeaderIcon, 0);
    lv_label_set_text(ic, st.icon);
    lv_obj_set_style_text_color(ic, C(st.color), 0);
    char buf[96];
    char age[16];
    ageText(age, sizeof age, m.ts);
    snprintf(buf, sizeof buf, "%s  ·  %s", st.label, age);
    lv_label_set_text(dHeaderSrc, buf);
    if (m.pri != 1) {
        lv_label_set_text(dPri, priorityLabel(m.pri));
        lv_obj_set_style_bg_color(dPri, C(priorityColor(m.pri)), 0);
        lv_obj_set_style_text_color(dPri, C(m.pri >= 2 ? 0x1A1206 : col::text), 0);
        lv_obj_align(dPri, LV_ALIGN_TOP_RIGHT, -10, 5);
        lv_obj_clear_flag(dPri, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(dPri, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(dSender, *m.sender ? m.sender : st.label);
    struct tm t;
    localTm(m.ts, &t);
    if (m.ts && ctx.now && ctx.now()) formatStamp(buf, sizeof buf, t);
    else buf[0] = 0;
    lv_label_set_text(dStamp, buf);
    lv_label_set_text(dSubject, *m.subject ? m.subject : "");
    if (*m.subject) lv_obj_clear_flag(dSubject, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(dSubject, LV_OBJ_FLAG_HIDDEN);
    if (m.flags & kTruncated) {
        static char body[kBodyLen + 32];
        snprintf(body, sizeof body, "%s…\n\n(shortened for this device)", m.body);
        lv_label_set_text(dText, body);
    } else {
        lv_label_set_text(dText, m.body);
    }
    if (m.ackAt) {
        lv_label_set_text(dFooter, LV_SYMBOL_OK "  Acknowledged");
        lv_obj_set_style_text_color(dFooter, C(col::ok), 0);
    } else {
        lv_label_set_text(dFooter, "Hold " LV_SYMBOL_RIGHT " to acknowledge   ·   hold " LV_SYMBOL_LEFT " back");
        lv_obj_set_style_text_color(dFooter, C(col::dim), 0);
    }
    unlockStore();
    return true;
}

// ------------------------------------------------------------------ settings

static void settingAction(int row);

static void settingClicked(lv_event_t* e) {
    settingAction(static_cast<int>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e))));
}

static void buildSettings() {
    lv_obj_t* s = screens[3] = newScreen();
    lv_obj_t* title = label(s, &sp_font_16, col::text, "Settings");
    lv_obj_set_pos(title, 12, 4);
    icons[3] = makeStatusIcons(s, 238, 6);
    sList = box(s, 0, 26, kScreenW, kScreenH - 26);
    lv_obj_add_flag(sList, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(sList, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sList, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(sList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(sList, 8, 0);
    lv_obj_set_style_pad_bottom(sList, 6, 0);
    lv_obj_set_style_pad_row(sList, 2, 0);
    for (int i = 0; i < kRowCount; ++i) {
        lv_obj_t* r = box(sList, 0, 0, kScreenW - 16, 28);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_add_style(r, &stRowFocus, LV_STATE_CHECKED);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, settingClicked, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        lv_obj_t* l = label(r, &sp_font_14, col::dim, kRowLabels[i]);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
        sValues[i] = label(r, &sp_font_14, col::text, "");
        lv_obj_set_width(sValues[i], 190);
        lv_label_set_long_mode(sValues[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(sValues[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(sValues[i], LV_ALIGN_RIGHT_MID, -10, 0);
        sRows[i] = r;
    }
}

static void refreshSettings(const DeviceStatus& st) {
    char b[96];
    if (st.wifiConnected) snprintf(b, sizeof b, "%s  %d dBm", st.ssid, st.rssi);
    else snprintf(b, sizeof b, "%s", *st.ssid ? "Disconnected" : "Not configured");
    lv_label_set_text(sValues[kRowWifi], b);
    lv_label_set_text(sValues[kRowIp], st.wifiConnected ? st.ip : "–");
    static const char* const kLink[] = {"offline", "connecting", "online", "auth error", "error"};
    const char* host = st.serverUrl;
    if (const char* p = strstr(host, "://")) host = p + 3;
    snprintf(b, sizeof b, "%s %s", st.server == Link::Online ? LV_SYMBOL_OK : "\xE2\x80\xA2", *host ? host : "not set");
    lv_label_set_text(sValues[kRowServer], b);
    lv_obj_set_style_text_color(sValues[kRowServer], C(linkColor(st.server)), 0);
    uint32_t now = ctx.now ? ctx.now() : 0;
    if (st.lastSync && now >= st.lastSync) {
        char d[16];
        formatDuration(d, sizeof d, now - st.lastSync);
        snprintf(b, sizeof b, "%s ago", d);
    } else {
        snprintf(b, sizeof b, "%s", *st.lastError ? st.lastError : kLink[static_cast<int>(st.server)]);
    }
    lv_label_set_text(sValues[kRowSync], b);
    snprintf(b, sizeof b, "%u %%  " LV_SYMBOL_RIGHT, st.brightness);
    lv_label_set_text(sValues[kRowBrightness], b);
    snprintf(b, sizeof b, "%u s%s  " LV_SYMBOL_RIGHT, st.pollSeconds, st.longPoll ? " · push" : "");
    lv_label_set_text(sValues[kRowPoll], b);
    lv_label_set_text(sValues[kRowSyncNow], LV_SYMBOL_REFRESH);
    snprintf(b, sizeof b, "%s", st.deviceId[0] ? st.deviceId : "not provisioned");
    lv_label_set_text(sValues[kRowDevice], b);
    lv_label_set_text(sValues[kRowTouch], st.touch ? "Detected" : "Not detected");
    snprintf(b, sizeof b, "%u KB heap · %u KB PSRAM", static_cast<unsigned>(st.freeHeap / 1024),
             static_cast<unsigned>(st.freePsram / 1024));
    lv_label_set_text(sValues[kRowMemory], b);
    formatDuration(b, sizeof b, st.uptimeS);
    lv_label_set_text(sValues[kRowUptime], b);
    lv_label_set_text(sValues[kRowFirmware], st.firmware);
    lv_label_set_text(sValues[kRowSetup], "Hold " LV_SYMBOL_RIGHT);
}

static void focusSettings(int idx, bool anim) {
    if (idx < 0) idx = 0;
    if (idx >= kRowCount) idx = kRowCount - 1;
    lv_obj_clear_state(sRows[sFocus], LV_STATE_CHECKED);
    sFocus = idx;
    lv_obj_add_state(sRows[sFocus], LV_STATE_CHECKED);
    lv_obj_scroll_to_view(sRows[sFocus], anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

static void settingAction(int row) {
    if (!ctx.onSetting) return;
    focusSettings(row, true);
    switch (row) {
        case kRowBrightness: ctx.onSetting(SettingAction::Brightness); break;
        case kRowPoll: ctx.onSetting(SettingAction::PollInterval); break;
        case kRowSyncNow: ctx.onSetting(SettingAction::SyncNow); break;
        case kRowSetup: ctx.onSetting(SettingAction::SetupMode); break;
        default: break;
    }
}

// ------------------------------------------------------------------ navigation

static void load(Screen s, bool forward) {
    if (s == current) return;
    current = s;
    lv_scr_load_anim(screens[static_cast<int>(s)], forward ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                     170, 0, false);
}

static void openDetail(uint32_t id) {
    dId = id;
    if (ctx.onRead) ctx.onRead(id);  // marks read locally and queues the event
    if (!refreshDetail()) return;
    lv_obj_scroll_to_y(dBody, 0, LV_ANIM_OFF);
    load(Screen::Detail, true);
}

static void openInbox(bool firstUnread) {
    if (firstUnread) {
        lockStore();
        size_t n = ctx.store->count();
        int idx = 0;
        for (size_t i = 0; i < n; ++i)
            if (!ctx.store->at(i).readAt) { idx = static_cast<int>(i); break; }
        unlockStore();
        focusInbox(idx, false);
    }
    load(Screen::Inbox, true);
}

static void gesture(lv_event_t*) {
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (d == LV_DIR_RIGHT) uiKey(Key::Back);
    else if (d == LV_DIR_LEFT && current == Screen::Home) openInbox(true);
}

void uiKey(Key k) {
    switch (current) {
        case Screen::Home:
            if (k == Key::Next || k == Key::Select) openInbox(true);
            else if (k == Key::Prev) { focusSettings(0, false); load(Screen::Settings, false); }
            break;
        case Screen::Inbox:
            if (k == Key::Prev) focusInbox(iFocus - 1, true);
            else if (k == Key::Next) focusInbox(iFocus + 1, true);
            else if (k == Key::Select && iRowCount) openDetail(iIds[iFocus]);
            else if (k == Key::Back) load(Screen::Home, false);
            break;
        case Screen::Detail:
            if (k == Key::Prev) lv_obj_scroll_by_bounded(dBody, 0, 60, LV_ANIM_ON);
            else if (k == Key::Next) lv_obj_scroll_by_bounded(dBody, 0, -60, LV_ANIM_ON);
            else if (k == Key::Select && ctx.onAck) ctx.onAck(dId);
            else if (k == Key::Back) load(Screen::Inbox, false);
            break;
        case Screen::Settings:
            if (k == Key::Prev) focusSettings(sFocus - 1, true);
            else if (k == Key::Next) focusSettings(sFocus + 1, true);
            else if (k == Key::Select) settingAction(sFocus);
            else if (k == Key::Back) load(Screen::Home, true);
            break;
    }
}

void uiShow(Screen s, int focus) {
    if (s == Screen::Inbox) focusInbox(focus, false);
    if (s == Screen::Settings) focusSettings(focus, false);
    if (s == Screen::Detail && focus < static_cast<int>(iRowCount)) {
        dId = iIds[focus];
        refreshDetail();
    }
    current = s;
    lv_scr_load(screens[static_cast<int>(s)]);
}

// ------------------------------------------------------------------ lifecycle

void uiInit(const UiContext& c) {
    ctx = c;
    lv_style_init(&stCard);
    lv_style_set_bg_color(&stCard, C(col::card));
    lv_style_set_bg_opa(&stCard, LV_OPA_COVER);
    lv_style_set_radius(&stCard, 14);

    lv_style_init(&stRowFocus);
    lv_style_set_bg_color(&stRowFocus, C(col::cardHi));
    lv_style_set_bg_opa(&stRowFocus, LV_OPA_COVER);
    lv_style_set_border_color(&stRowFocus, C(col::accent));
    lv_style_set_border_width(&stRowFocus, 2);
    lv_style_set_border_opa(&stRowFocus, LV_OPA_COVER);

    lv_style_init(&stChip);
    lv_style_set_radius(&stChip, 8);
    lv_style_set_bg_opa(&stChip, LV_OPA_COVER);
    lv_style_set_pad_hor(&stChip, 7);
    lv_style_set_pad_ver(&stChip, 2);

    buildHome();
    buildInbox();
    buildDetail();
    buildSettings();
    buildToast();
    for (lv_obj_t* s : screens) {
        lv_obj_add_event_cb(s, gesture, LV_EVENT_GESTURE, nullptr);
    }
    lv_obj_add_state(sRows[0], LV_STATE_CHECKED);
    shownVersion = 0;
    shownMinute = -1;
    memset(&shownStatus, 0xFF, sizeof shownStatus);
    current = Screen::Home;
    lv_scr_load(screens[0]);
    uiTick();
}

void uiTick() {
    uint32_t now = ctx.now ? ctx.now() : 0;
    int minute = now ? static_cast<int>(now / 60) : -2;
    bool minuteChanged = minute != shownMinute;
    if (minuteChanged) {
        shownMinute = minute;
        refreshClock();
    }
    lockStore();
    uint32_t v = ctx.store->version();
    unlockStore();
    if (v != shownVersion || minuteChanged) {  // ages ("5m") change every minute
        shownVersion = v;
        refreshHome();
        refreshInbox();
        if (current == Screen::Detail && !refreshDetail()) load(Screen::Inbox, false);
    }
    // status changes often (uptime, heap): compare and redraw at most once per second
    if (memcmp(&shownStatus, ctx.status, sizeof shownStatus) != 0 && (now != lastStatusRefresh || minuteChanged)) {
        lastStatusRefresh = now;
        shownStatus = *ctx.status;
        updateStatusIcons(shownStatus);
        refreshHomeFooter(shownStatus);
        refreshSettings(shownStatus);
    }
}

}  // namespace sp
