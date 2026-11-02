// Text formatting helpers for the UI. Pure C++ so they are unit-tested on the host.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace sp {

// "now", "5m", "3h", "Yest", "Mon", "2 Nov" relative to `now` (both unix seconds).
// Calendar-based labels use the local broken-down times supplied by the caller.
void formatAge(char* out, size_t cap, uint32_t now, uint32_t ts, const struct tm& nowLocal,
               const struct tm& tsLocal);

// "Monday, November 2"
void formatLongDate(char* out, size_t cap, const struct tm& t);
// "14:35" / "Mon 2 Nov 14:35"
void formatClock(char* out, size_t cap, const struct tm& t);
void formatStamp(char* out, size_t cap, const struct tm& t);

struct SourceStyle {
    const char* label;   // short, user-facing
    const char* icon;    // LV_SYMBOL_* UTF-8 string
    uint32_t color;      // 0xRRGGBB accent
};
const SourceStyle& sourceStyle(const char* src);

const char* priorityLabel(uint8_t pri);
uint32_t priorityColor(uint8_t pri);

// Title for list rows: subject, else the first line of the body, else "(no subject)".
void previewTitle(char* out, size_t cap, const char* subject, const char* body);

// Formats a duration in seconds as "45 s", "12 min", "3 h", "2 d".
void formatDuration(char* out, size_t cap, uint32_t seconds);

}  // namespace sp
