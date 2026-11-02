/* LVGL 8.3 configuration for SmartPager. Unlisted options use LVGL defaults
 * (lv_conf_internal.h). Shared by the device build and the host simulator. */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH 16
#ifdef SMARTPAGER_HOST
#define LV_COLOR_16_SWAP 0
#else
/* The ST7789 on the 8-bit i80 bus expects big-endian RGB565 (same as LILYGO's lv_conf.h). */
#define LV_COLOR_16_SWAP 1
#endif
#define LV_COLOR_SCREEN_TRANSP 0
#define LV_COLOR_CHROMA_KEY lv_color_hex(0x00ff00)

/* Use the C library allocator: on the ESP32-S3 with PSRAM, Arduino's malloc places
 * allocations above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KiB) in PSRAM. */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC malloc
#define LV_MEM_CUSTOM_FREE free
#define LV_MEM_CUSTOM_REALLOC realloc
#define LV_MEM_BUF_MAX_NUM 16
#define LV_MEMCPY_MEMSET_STD 1

#define LV_DISP_DEF_REFR_PERIOD 20
#define LV_INDEV_DEF_READ_PERIOD 20
#define LV_DPI_DEF 200

#ifdef SMARTPAGER_HOST
#define LV_TICK_CUSTOM 0
#else
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#endif

#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE 0
#define LV_CIRCLE_CACHE_SIZE 4
#define LV_IMG_CACHE_DEF_SIZE 0
#define LV_GRADIENT_MAX_STOPS 2
#define LV_GRAD_CACHE_DEF_SIZE 0
#define LV_DISP_ROT_MAX_BUF (10 * 1024)

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_SPRINTF_CUSTOM 0
#define LV_SPRINTF_USE_FLOAT 0
#define LV_USE_USER_DATA 1

#define LV_ATTRIBUTE_FAST_MEM
#define LV_ATTRIBUTE_LARGE_CONST

/* Fonts: only our generated Inter fonts (with Czech glyphs) are used; Montserrat 14 is
 * kept as LVGL's default to satisfy internal references. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_CUSTOM_DECLARE                                                       \
    LV_FONT_DECLARE(sp_font_12) LV_FONT_DECLARE(sp_font_14) LV_FONT_DECLARE(sp_font_16) \
    LV_FONT_DECLARE(sp_font_22) LV_FONT_DECLARE(sp_font_clock)
#define LV_FONT_DEFAULT &sp_font_14
#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_SUBPX 0

#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_/"
#define LV_TXT_LINE_BREAK_LONG_LEN 12
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/* Widgets actually used */
#define LV_USE_ARC 0
#define LV_USE_BAR 1
#define LV_USE_BTN 0
#define LV_USE_BTNMATRIX 0
#define LV_USE_CANVAS 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMG 0
#define LV_USE_LABEL 1
#define LV_LABEL_TEXT_SELECTION 0
#define LV_LABEL_LONG_TXT_HINT 1
#define LV_USE_LINE 0
#define LV_USE_ROLLER 0
#define LV_USE_SLIDER 0
#define LV_USE_SWITCH 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TABLE 0

/* Extra components: all off */
#define LV_USE_ANIMIMG 0
#define LV_USE_CALENDAR 0
#define LV_USE_CHART 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_METER 0
#define LV_USE_MSGBOX 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_BASIC 0
#define LV_USE_THEME_MONO 0
#define LV_USE_FLEX 1
#define LV_USE_GRID 0
#define LV_USE_SNAPSHOT 0
#define LV_BUILD_EXAMPLES 0

#endif /* LV_CONF_H */
