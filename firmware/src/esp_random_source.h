#pragma once

// auth::RandomSource on the ESP32 hardware RNG.
//
// esp_fill_random() is cryptographically secure only while an entropy source
// runs: the RF subsystem (BLE here), per ESP-IDF's random number docs. Until
// main marks it ready (after BLE starts), fill() fails, so session ids and
// tokens fail closed instead of being made from weak randomness. If BLE
// never starts, logins and token creation stay refused.

#include <esp_random.h>

#include <atomic>

#include "hubbo/auth/crypto.h"

namespace hubbo::fw {

class EspRandomSource : public hubbo::auth::RandomSource {
public:
    void setEntropyReady(bool ready) { ready_.store(ready, std::memory_order_release); }

    bool fill(uint8_t *out, size_t length) override {
        if (!ready_.load(std::memory_order_acquire)) {
            return false;
        }
        esp_fill_random(out, length);
        return true;
    }

private:
    std::atomic<bool> ready_{false};
};

}  // namespace hubbo::fw
