#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "hubbo/api/authenticator.h"
#include "hubbo/auth/crypto.h"
#include "hubbo/http/route.h"

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

// Request headers for tests; names match case-insensitively.
struct Headers : http::HeaderSource {
    std::map<std::string, std::string> items;
    std::string_view header(std::string_view name) const override {
        for (const auto &h : items) {
            if (asciiEqualsIgnoreCase(h.first, name)) return h.second;
        }
        return {};
    }
};

struct FakeClock : api::Clock {
    uint64_t mono = 1000;
    int64_t unix = 1762600000;
    uint64_t monotonicMs() const override { return mono; }
    int64_t unixSeconds() const override { return unix; }
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
