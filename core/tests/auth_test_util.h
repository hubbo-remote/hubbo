#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "hubbo/auth/crypto.h"

namespace hubbo::auth::test {

// Deterministic, distinct bytes on every call. Not random; tests only.
class CountingRandom : public RandomSource {
public:
    bool fill(uint8_t *out, size_t length) override {
        if (failing) return false;
        for (size_t i = 0; i < length; i++) {
            counter = counter * 6364136223846793005ull + 1442695040888963407ull;
            out[i] = static_cast<uint8_t>(counter >> 56);
        }
        return true;
    }
    uint64_t counter = 1;
    bool failing = false;
};

inline std::string hex(const uint8_t *data, size_t length) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < length; i++) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

}  // namespace hubbo::auth::test
