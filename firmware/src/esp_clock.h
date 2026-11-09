#pragma once

// api::Clock for the ESP32. Monotonic time from esp_timer (64-bit, never
// wraps in practice). Wall-clock time is unknown (-1) until something sets
// the system time; SNTP isn't started yet.

#include <esp_timer.h>

#include <ctime>

#include "hubbo/api/authenticator.h"

namespace hubbo::fw {

class EspClock : public hubbo::api::Clock {
public:
    uint64_t monotonicMs() const override { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

    int64_t unixSeconds() const override {
        constexpr time_t kEarliestPlausible = 1735689600;  // 2025-01-01: anything before is unset
        const time_t now = time(nullptr);
        return now >= kEarliestPlausible ? static_cast<int64_t>(now) : -1;
    }
};

}  // namespace hubbo::fw
