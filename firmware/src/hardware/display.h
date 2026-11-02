#pragma once

#include <stdint.h>

namespace display {
// Initializes the ST7789 over the 8-bit i80 bus, LVGL and its display driver (landscape 320x170).
bool begin(bool flip);
// 0..100 %, mapped to the backlight driver's 16 steps. 0 turns it off.
void setBrightness(uint8_t percent);
void sleep(bool on);
}  // namespace display
