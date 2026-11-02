#pragma once

#include "ui/ui.h"

namespace input {
// Buttons are always available; touch is registered with LVGL only if a controller answers.
void begin(bool flip);
bool touchPresent();
// Polls the buttons; returns true and sets `key` when a navigation event happened.
bool poll(sp::Key& key);
// True while the KEY (GPIO14) button is held — used at boot to enter setup mode.
bool keyHeldRaw();
}  // namespace input
