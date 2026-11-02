// Display / input verification firmware (pio run -e diag -t upload).
// Shows colour bars and orientation markers, the I2C scan result (touch controller),
// PSRAM/flash sizes and live button + touch state. Uses no Wi-Fi and stores nothing.
#include <Arduino.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include <lvgl.h>

#include "hardware/board.h"
#include "hardware/display.h"
#include "hardware/input.h"
#include "ui/ui.h"

static lv_obj_t *keyLabel, *touchLabel;
static uint32_t presses[4];

static lv_obj_t* text(lv_obj_t* parent, const lv_font_t* f, uint32_t color, int x, int y, const char* t) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, t);
    lv_obj_set_pos(l, x, y);
    return l;
}

void setup() {
    Serial.begin(115200);
    display::begin(false);
    input::begin(false);
    display::setBrightness(80);

    lv_obj_t* s = lv_scr_act();
    lv_obj_set_style_bg_color(s, lv_color_hex(0x0A0B10), 0);
    // colour bars: red, green, blue, white — wrong order/colours mean wrong colour config
    const uint32_t bars[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* b = lv_obj_create(s);
        lv_obj_remove_style_all(b);
        lv_obj_set_style_bg_color(b, lv_color_hex(bars[i]), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_size(b, 80, 14);
        lv_obj_set_pos(b, i * 80, 0);
    }
    text(s, &sp_font_12, 0xFFB020, 2, 16, "TOP-LEFT");
    lv_obj_t* br = text(s, &sp_font_12, 0xFFB020, 0, 0, "BOTTOM-RIGHT");
    lv_obj_align(br, LV_ALIGN_BOTTOM_RIGHT, -2, -2);

    char buf[160];
    snprintf(buf, sizeof buf, "%s rev %d  ·  flash %u MB  ·  PSRAM %u KB", ESP.getChipModel(), ESP.getChipRevision(),
             ESP.getFlashChipSize() >> 20, ESP.getPsramSize() >> 10);
    text(s, &sp_font_12, 0xB4BACB, 8, 36, buf);

    String found = "I2C:";
    for (uint8_t a = 1; a < 127; ++a) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) found += String(" 0x") + String(a, HEX);
    }
    if (found == "I2C:") found += " none";
    found += input::touchPresent() ? "  (CST816 touch OK)" : "";
    text(s, &sp_font_12, 0xB4BACB, 8, 54, found.c_str());
    Serial.printf("[diag] %s\n[diag] %s\n", buf, found.c_str());

    text(s, &sp_font_16, 0xF3F4F8, 8, 78, "Příliš žluťoučký kůň úpěl");
    keyLabel = text(s, &sp_font_14, 0x9B8CFF, 8, 104, "Press BOOT / KEY (short and long)");
    touchLabel = text(s, &sp_font_12, 0x7A8194, 8, 128, input::touchPresent() ? "Touch the screen" : "No touch controller");
}

void loop() {
    lv_timer_handler();
    sp::Key k;
    if (input::poll(k)) {
        static const char* const names[] = {"BOOT short (Prev)", "KEY short (Next)", "KEY long (Select)", "BOOT long (Back)"};
        int i = static_cast<int>(k);
        ++presses[i];
        char b[96];
        snprintf(b, sizeof b, "%s  ·  counts %u/%u/%u/%u", names[i], presses[0], presses[1], presses[2], presses[3]);
        lv_label_set_text(keyLabel, b);
        Serial.printf("[diag] button %s\n", names[i]);
    }
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    if (indev && indev->proc.state == LV_INDEV_STATE_PRESSED) {
        char b[48];
        snprintf(b, sizeof b, "Touch x=%d y=%d", indev->proc.types.pointer.act_point.x, indev->proc.types.pointer.act_point.y);
        lv_label_set_text(touchLabel, b);
    }
    delay(5);
}
