#pragma once

#include <cstddef>
#include <cstdint>

// Failed-login throttling (also used for password change).
//
// Per client (keyed by the caller, e.g. IPv4 address or a hash of the IPv6
// /64): the first `freeFailures` failures cost nothing; each further failure
// doubles a lockout starting at `baseLockoutMs`, capped at `maxLockoutMs`.
// A client's record is forgotten after `forgetAfterMs` without failures, and
// a successful login clears it.
//
// Globally: more than `globalMaxFailures` failures in a fixed
// `globalWindowMs` window locks everyone out until the window ends, so
// rotating source addresses does not buy unlimited guesses.
//
// Fixed table (kSlots); when full, the stalest record is recycled. Times are
// a monotonic millisecond clock.
namespace hubbo::auth {

struct RateLimitConfig {
    uint32_t freeFailures = 5;
    uint64_t baseLockoutMs = 1000;
    uint64_t maxLockoutMs = 15ull * 60 * 1000;
    uint64_t forgetAfterMs = 60ull * 60 * 1000;
    uint64_t globalWindowMs = 60ull * 1000;
    uint32_t globalMaxFailures = 30;
};

class LoginRateLimiter {
public:
    static constexpr size_t kSlots = 16;

    explicit LoginRateLimiter(RateLimitConfig config = {}) : config_(config) {}

    // 0 if the client may try now, otherwise milliseconds until it may.
    // Check before verifying the password; answer 429 with Retry-After.
    uint64_t retryAfterMs(uint64_t clientKey, uint64_t nowMs) const;

    void recordFailure(uint64_t clientKey, uint64_t nowMs);
    void recordSuccess(uint64_t clientKey);

private:
    struct Slot {
        bool used = false;
        uint64_t key = 0;
        uint32_t failures = 0;
        uint64_t lastFailureMs = 0;
        uint64_t lockedUntilMs = 0;
    };

    int find(uint64_t key, uint64_t nowMs) const;

    RateLimitConfig config_;
    Slot slots_[kSlots];
    uint64_t windowStartMs_ = 0;
    uint32_t windowFailures_ = 0;
};

}  // namespace hubbo::auth
