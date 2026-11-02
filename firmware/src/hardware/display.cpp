// ST7789V (170x320) on the ESP32-S3 LCD peripheral (i80, 8-bit) — the same driver path and
// init sequence as LILYGO's factory firmware, wired to LVGL 8.3 with partial DMA buffers.
#include "display.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <lvgl.h>

#include "board.h"
#include "ui/ui.h"

namespace display {
namespace {

esp_lcd_panel_handle_t panel = nullptr;
esp_lcd_panel_io_handle_t io = nullptr;
lv_disp_draw_buf_t drawBuf;
lv_disp_drv_t drv;
constexpr int kBufLines = 34;  // 2 x 320*34*2 B = 43.5 KB of internal DMA RAM

struct LcdCmd {
    uint8_t cmd;
    uint8_t data[14];
    uint8_t len;  // bit 7: delay 120 ms after
};
// Panel tuning for the current LILYGO module revision (factory.ino LCD_MODULE_CMD_1).
const LcdCmd kInit[] = {
    {0x11, {0}, 0 | 0x80},
    {0x3A, {0x05}, 1},
    {0xB2, {0x0B, 0x0B, 0x00, 0x33, 0x33}, 5},
    {0xB7, {0x75}, 1},
    {0xBB, {0x28}, 1},
    {0xC0, {0x2C}, 1},
    {0xC2, {0x01}, 1},
    {0xC3, {0x1F}, 1},
    {0xC6, {0x13}, 1},
    {0xD0, {0xA7}, 1},
    {0xD0, {0xA4, 0xA1}, 2},
    {0xD6, {0xA1}, 1},
    {0xE0, {0xF0, 0x05, 0x0A, 0x06, 0x06, 0x03, 0x2B, 0x32, 0x43, 0x36, 0x11, 0x10, 0x2B, 0x32}, 14},
    {0xE1, {0xF0, 0x08, 0x0C, 0x0B, 0x09, 0x24, 0x2B, 0x22, 0x43, 0x38, 0x15, 0x16, 0x2F, 0x37}, 14},
};

bool IRAM_ATTR flushDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* user) {
    lv_disp_flush_ready(static_cast<lv_disp_drv_t*>(user));
    return false;
}

void flush(lv_disp_drv_t*, const lv_area_t* a, lv_color_t* px) {
    esp_lcd_panel_draw_bitmap(panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
}

uint8_t level = 0;  // backlight driver step currently set (0 = off, 16 = max)

}  // namespace

// The backlight chip counts falling edges: each pulse steps the brightness down by one of
// 16 levels, wrapping around (LILYGO factory.ino setBrightness()).
static void setLevel(uint8_t value) {
    constexpr uint8_t steps = 16;
    if (value == 0) {
        digitalWrite(board::kLcdBacklight, 0);
        delay(3);
        level = 0;
        return;
    }
    if (level == 0) {
        digitalWrite(board::kLcdBacklight, 1);
        level = steps;
        delayMicroseconds(30);
    }
    int from = steps - level, to = steps - value;
    int num = (steps + to - from) % steps;
    for (int i = 0; i < num; ++i) {
        digitalWrite(board::kLcdBacklight, 0);
        digitalWrite(board::kLcdBacklight, 1);
    }
    level = value;
}

void setBrightness(uint8_t percent) {
    if (percent > 100) percent = 100;
    uint8_t v = percent == 0 ? 0 : static_cast<uint8_t>(1 + (percent * 15 + 50) / 100);
    if (v != level) setLevel(v);
}

void sleep(bool on) {
    if (!panel) return;
    if (on) {
        setLevel(0);
        esp_lcd_panel_io_tx_param(io, 0x10, nullptr, 0);  // SLPIN
    } else {
        esp_lcd_panel_io_tx_param(io, 0x11, nullptr, 0);  // SLPOUT
        delay(120);
    }
}

bool begin(bool flip) {
    pinMode(board::kPowerOn, OUTPUT);
    digitalWrite(board::kPowerOn, HIGH);
    pinMode(board::kLcdRd, OUTPUT);
    digitalWrite(board::kLcdRd, HIGH);
    pinMode(board::kLcdBacklight, OUTPUT);
    digitalWrite(board::kLcdBacklight, LOW);

    esp_lcd_i80_bus_handle_t bus = nullptr;
    esp_lcd_i80_bus_config_t busCfg = {};
    busCfg.dc_gpio_num = board::kLcdDc;
    busCfg.wr_gpio_num = board::kLcdWr;
    busCfg.clk_src = LCD_CLK_SRC_PLL160M;
    for (int i = 0; i < 8; ++i) busCfg.data_gpio_nums[i] = board::kLcdD[i];
    busCfg.bus_width = 8;
    busCfg.max_transfer_bytes = sp::kScreenW * kBufLines * sizeof(uint16_t);
    if (esp_lcd_new_i80_bus(&busCfg, &bus) != ESP_OK) return false;

    esp_lcd_panel_io_i80_config_t ioCfg = {};
    ioCfg.cs_gpio_num = board::kLcdCs;
    ioCfg.pclk_hz = board::kLcdPixelClockHz;
    ioCfg.trans_queue_depth = 10;
    ioCfg.on_color_trans_done = flushDone;
    ioCfg.user_ctx = &drv;
    ioCfg.lcd_cmd_bits = 8;
    ioCfg.lcd_param_bits = 8;
    ioCfg.dc_levels.dc_data_level = 1;
    if (esp_lcd_new_panel_io_i80(bus, &ioCfg, &io) != ESP_OK) return false;

    esp_lcd_panel_dev_config_t pCfg = {};
    pCfg.reset_gpio_num = board::kLcdReset;
    pCfg.color_space = ESP_LCD_COLOR_SPACE_RGB;
    pCfg.bits_per_pixel = 16;
    if (esp_lcd_new_panel_st7789(io, &pCfg, &panel) != ESP_OK) return false;
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_swap_xy(panel, true);
    // factory default: USB connector on the left; `flip` rotates the image 180 degrees
    if (flip) esp_lcd_panel_mirror(panel, true, false);
    else esp_lcd_panel_mirror(panel, false, true);
    esp_lcd_panel_set_gap(panel, 0, board::kLcdGapY);
    for (const LcdCmd& c : kInit) {
        esp_lcd_panel_io_tx_param(io, c.cmd, c.data, c.len & 0x7F);
        if (c.len & 0x80) delay(120);
    }
    esp_lcd_panel_disp_on_off(panel, true);

    lv_init();
    size_t bytes = sp::kScreenW * kBufLines * sizeof(lv_color_t);
    auto* b1 = static_cast<lv_color_t*>(heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    auto* b2 = static_cast<lv_color_t*>(heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!b1 || !b2) return false;
    lv_disp_draw_buf_init(&drawBuf, b1, b2, sp::kScreenW * kBufLines);
    lv_disp_drv_init(&drv);
    drv.hor_res = sp::kScreenW;
    drv.ver_res = sp::kScreenH;
    drv.flush_cb = flush;
    drv.draw_buf = &drawBuf;
    lv_disp_drv_register(&drv);
    return true;
}

}  // namespace display
