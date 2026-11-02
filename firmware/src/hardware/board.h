// LILYGO T-Display-S3 (and T-Display-S3 Touch) pin map.
// Source: Xinyuan-LilyGO/T-Display-S3 examples/factory/pin_config.h (verified 2026-11-02).
#pragma once

namespace board {
constexpr int kPowerOn = 15;   // must be HIGH to power the LCD when running from battery
constexpr int kLcdBacklight = 38;
constexpr int kLcdD[8] = {39, 40, 41, 42, 45, 46, 47, 48};
constexpr int kLcdReset = 5;
constexpr int kLcdCs = 6;
constexpr int kLcdDc = 7;
constexpr int kLcdWr = 8;
constexpr int kLcdRd = 9;
constexpr int kLcdPixelClockHz = 16 * 1000 * 1000;  // LILYGO: higher clocks cause mosaic artefacts
constexpr int kLcdNativeW = 170;
constexpr int kLcdNativeH = 320;
constexpr int kLcdGapY = 35;  // panel-specific column offset after swap_xy

constexpr int kButtonBoot = 0;  // also the ROM download-mode strap: never require it held at reset
constexpr int kButtonKey = 14;
constexpr int kBatteryAdc = 4;  // through a 1:2 divider

constexpr int kI2cSda = 18;
constexpr int kI2cScl = 17;
constexpr int kTouchInt = 16;
constexpr int kTouchReset = 21;
constexpr int kTouchAddrCst816 = 0x15;  // current Touch variant (self-capacitive)
constexpr int kTouchAddrCst328 = 0x1A;  // early Touch variant (mutual-capacitive)
}  // namespace board
