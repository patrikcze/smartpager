#include "input.h"

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "board.h"
#include "messaging/policies.h"

namespace input {
namespace {

sp::ButtonFsm prevBtn, nextBtn;  // BOOT = previous/back, KEY = next/select
bool touch = false;
bool flipped = false;
lv_indev_drv_t indevDrv;
// The CST816 sleeps between touches and NACKs I2C reads, so it is only read after it
// signalled activity on INT (falling edge), and then for as long as a finger is reported.
volatile bool touchIrq = false;
bool fingerDown = false;

void IRAM_ATTR onTouchIrq() { touchIrq = true; }

bool probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// CST816 register map (LILYGO TouchLib ModulesCSTSelf.tpp): 0x02 = finger count,
// 0x03..0x06 = X/Y high nibble + low byte, in native portrait coordinates (170x320).
void readTouch(lv_indev_drv_t*, lv_indev_data_t* data) {
    static lv_point_t last = {0, 0};
    data->point = last;
    data->state = LV_INDEV_STATE_REL;
    if (!touchIrq && !fingerDown) return;
    touchIrq = false;
    uint8_t buf[7] = {0};
    Wire.beginTransmission(board::kTouchAddrCst816);
    Wire.write(0x00);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(board::kTouchAddrCst816, 7) != 7) {
        fingerDown = false;
        return;
    }
    for (uint8_t& b : buf) b = Wire.read();
    fingerDown = (buf[2] & 0x0F) != 0;
    if (!fingerDown) return;
    int rawX = ((buf[3] & 0x0F) << 8) | buf[4];
    int rawY = ((buf[5] & 0x0F) << 8) | buf[6];
    // Landscape (USB left): screen x = native y, screen y = native x (factory.ino rotation 1).
    int x = rawY, y = rawX;
    if (flipped) {
        x = sp::kScreenW - 1 - x;
        y = sp::kScreenH - 1 - y;
    }
    last.x = constrain(x, 0, sp::kScreenW - 1);
    last.y = constrain(y, 0, sp::kScreenH - 1);
    data->point = last;
    data->state = LV_INDEV_STATE_PR;
}

}  // namespace

void begin(bool flip) {
    flipped = flip;
    pinMode(board::kButtonBoot, INPUT_PULLUP);
    pinMode(board::kButtonKey, INPUT_PULLUP);
    // A button still held from power-up (e.g. KEY for setup mode) must not navigate.
    if (digitalRead(board::kButtonBoot) == LOW) prevBtn.suppressUntilRelease();
    if (digitalRead(board::kButtonKey) == LOW) nextBtn.suppressUntilRelease();

    pinMode(board::kTouchReset, OUTPUT);
    digitalWrite(board::kTouchReset, LOW);
    delay(10);
    digitalWrite(board::kTouchReset, HIGH);
    delay(60);
    Wire.begin(board::kI2cSda, board::kI2cScl, 400000);
    Wire.setTimeOut(20);
    if (probe(board::kTouchAddrCst816)) {
        touch = true;
        pinMode(board::kTouchInt, INPUT_PULLUP);
        attachInterrupt(board::kTouchInt, onTouchIrq, FALLING);
        lv_indev_drv_init(&indevDrv);
        indevDrv.type = LV_INDEV_TYPE_POINTER;
        indevDrv.read_cb = readTouch;
        lv_indev_drv_register(&indevDrv);
        log_i("touch: CST816 at 0x15");
    } else if (probe(board::kTouchAddrCst328)) {
        log_w("touch: CST328 at 0x1A found but not supported by this firmware; buttons only");
    } else {
        log_i("touch: none detected");
    }
}

bool touchPresent() { return touch; }

bool keyHeldRaw() { return digitalRead(board::kButtonKey) == LOW; }

bool poll(sp::Key& key) {
    uint32_t now = millis();
    sp::ButtonEvent a = prevBtn.update(digitalRead(board::kButtonBoot) == LOW, now);
    sp::ButtonEvent b = nextBtn.update(digitalRead(board::kButtonKey) == LOW, now);
    if (a == sp::ButtonEvent::Short) { key = sp::Key::Prev; return true; }
    if (a == sp::ButtonEvent::Long) { key = sp::Key::Back; return true; }
    if (b == sp::ButtonEvent::Short) { key = sp::Key::Next; return true; }
    if (b == sp::ButtonEvent::Long) { key = sp::Key::Select; return true; }
    return false;
}

}  // namespace input
