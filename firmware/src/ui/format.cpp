#include "format.h"

#include <stdio.h>
#include <string.h>

#include "../messaging/message_store.h"

namespace sp {

namespace {
const char* const kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* const kMonths[] = {"January", "February", "March", "April", "May", "June", "July",
                               "August", "September", "October", "November", "December"};

// UTF-8 encodings of the FontAwesome code points embedded in our fonts (LV_SYMBOL_*).
#define SYM_ENVELOPE "\xEF\x83\xA0"
#define SYM_BELL "\xEF\x83\xB3"
#define SYM_WARNING "\xEF\x81\xB1"
#define SYM_SETTINGS "\xEF\x80\x93"
#define SYM_COMMENT "\xEF\x81\xB5"
#define SYM_USER "\xEF\x80\x87"
#define SYM_CLOUD "\xEF\x83\x82"

const SourceStyle kSources[] = {
    {"Mail", SYM_ENVELOPE, 0x5B9DFF},     {"Message", SYM_COMMENT, 0x34C77B},
    {"Monitor", SYM_WARNING, 0xFF8A3D},   {"System", SYM_SETTINGS, 0xA78BFA},
    {"Webhook", SYM_CLOUD, 0x22C3E6},     {"Note", SYM_USER, 0xF5C451},
};
const char* const kSourceKeys[] = {"email", "imessage", "monitoring", "system", "webhook", "manual"};
}  // namespace

void formatAge(char* out, size_t cap, uint32_t now, uint32_t ts, const struct tm& nl, const struct tm& tl) {
    if (ts == 0) { snprintf(out, cap, "–"); return; }
    uint32_t d = now > ts ? now - ts : 0;
    if (d < 60) snprintf(out, cap, "now");
    else if (d < 3600) snprintf(out, cap, "%um", static_cast<unsigned>(d / 60));
    else if (d < 6 * 3600 || (nl.tm_yday == tl.tm_yday && nl.tm_year == tl.tm_year))
        snprintf(out, cap, "%uh", static_cast<unsigned>(d / 3600));
    else if (d < 2 * 86400 && (nl.tm_yday == tl.tm_yday + 1 || (nl.tm_yday == 0 && tl.tm_mon == 11)))
        snprintf(out, cap, "Yest");
    else if (d < 6 * 86400) snprintf(out, cap, "%.3s", kDays[tl.tm_wday % 7]);
    else snprintf(out, cap, "%d %.3s", tl.tm_mday, kMonths[tl.tm_mon % 12]);
}

void formatLongDate(char* out, size_t cap, const struct tm& t) {
    snprintf(out, cap, "%s, %s %d", kDays[t.tm_wday % 7], kMonths[t.tm_mon % 12], t.tm_mday);
}

void formatClock(char* out, size_t cap, const struct tm& t) {
    snprintf(out, cap, "%02d:%02d", t.tm_hour, t.tm_min);
}

void formatStamp(char* out, size_t cap, const struct tm& t) {
    snprintf(out, cap, "%.3s %d %.3s, %02d:%02d", kDays[t.tm_wday % 7], t.tm_mday, kMonths[t.tm_mon % 12],
             t.tm_hour, t.tm_min);
}

const SourceStyle& sourceStyle(const char* src) {
    for (size_t i = 0; i < sizeof kSourceKeys / sizeof kSourceKeys[0]; ++i)
        if (src && strcmp(src, kSourceKeys[i]) == 0) return kSources[i];
    return kSources[5];
}

const char* priorityLabel(uint8_t pri) {
    static const char* const k[] = {"Low", "Normal", "High", "Critical"};
    return k[pri > 3 ? 3 : pri];
}

uint32_t priorityColor(uint8_t pri) {
    static const uint32_t k[] = {0x7C8496, 0x9AA3B5, 0xFFB020, 0xFF4D5E};
    return k[pri > 3 ? 3 : pri];
}

void previewTitle(char* out, size_t cap, const char* subject, const char* body) {
    if (subject && *subject) { copyUtf8(out, cap, subject); return; }
    if (body && *body) {
        while (*body == ' ' || *body == '\n') ++body;
        const char* nl = strchr(body, '\n');
        size_t n = nl ? static_cast<size_t>(nl - body) + 1 : cap;
        copyUtf8(out, n < cap ? n : cap, body);
        return;
    }
    copyUtf8(out, cap, "(no subject)");
}

void formatDuration(char* out, size_t cap, uint32_t s) {
    if (s < 60) snprintf(out, cap, "%u s", static_cast<unsigned>(s));
    else if (s < 3600) snprintf(out, cap, "%u min", static_cast<unsigned>(s / 60));
    else if (s < 86400) snprintf(out, cap, "%u h", static_cast<unsigned>(s / 3600));
    else snprintf(out, cap, "%u d", static_cast<unsigned>(s / 86400));
}

}  // namespace sp
