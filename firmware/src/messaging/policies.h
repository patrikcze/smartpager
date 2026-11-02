// Small, pure state machines shared by the network task and the input layer.
#pragma once

#include <stdint.h>

namespace sp {

// Exponential backoff with "equal jitter": delay in [d/2, d), d doubling from base to cap.
class Backoff {
public:
    Backoff(uint32_t baseMs, uint32_t capMs) : base_(baseMs), cap_(capMs) {}
    uint32_t next(uint32_t random) {
        uint32_t d = base_;
        for (uint32_t i = 0; i < attempt_ && d < cap_; ++i) d = (d > cap_ / 2) ? cap_ : d * 2;
        if (d > cap_) d = cap_;
        if (attempt_ < 31) ++attempt_;
        uint32_t half = d / 2;
        return half + (half ? random % half : 0);
    }
    void reset() { attempt_ = 0; }
    uint32_t attempts() const { return attempt_; }

private:
    uint32_t base_, cap_, attempt_ = 0;
};

// Debounced push button with short-press (on release) and long-press (fired while held).
enum class ButtonEvent : uint8_t { None, Short, Long };

class ButtonFsm {
public:
    static constexpr uint32_t kDebounceMs = 25;
    static constexpr uint32_t kLongMs = 550;

    // `pressed` is the raw (already polarity-corrected) level, sampled every few ms.
    ButtonEvent update(bool pressed, uint32_t nowMs) {
        if (pressed != raw_) {
            raw_ = pressed;
            changedAt_ = nowMs;
        }
        if (nowMs - changedAt_ < kDebounceMs) return ButtonEvent::None;
        if (raw_ != stable_) {
            stable_ = raw_;
            if (stable_) {
                downAt_ = nowMs;
                longFired_ = false;
            } else if (!longFired_ && !suppressed_) {
                return ButtonEvent::Short;
            } else {
                suppressed_ = false;
            }
        }
        if (stable_ && !longFired_ && !suppressed_ && nowMs - downAt_ >= kLongMs) {
            longFired_ = true;
            return ButtonEvent::Long;
        }
        return ButtonEvent::None;
    }
    // Ignore the current press entirely (e.g. button still held from boot).
    void suppressUntilRelease() { suppressed_ = true; }
    bool held() const { return stable_; }

private:
    bool raw_ = false, stable_ = false, longFired_ = false, suppressed_ = false;
    uint32_t changedAt_ = 0, downAt_ = 0;
};

}  // namespace sp
