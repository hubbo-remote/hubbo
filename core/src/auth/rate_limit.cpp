#include "hubbo/auth/rate_limit.h"

namespace hubbo::auth {

int LoginRateLimiter::find(uint64_t key, uint64_t nowMs) const {
    for (size_t i = 0; i < kSlots; i++) {
        const Slot &slot = slots_[i];
        if (slot.used && slot.key == key && nowMs >= slot.lastFailureMs &&
            nowMs - slot.lastFailureMs < config_.forgetAfterMs) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

uint64_t LoginRateLimiter::retryAfterMs(uint64_t clientKey, uint64_t nowMs) const {
    uint64_t wait = 0;
    if (windowFailures_ >= config_.globalMaxFailures && nowMs >= windowStartMs_ &&
        nowMs - windowStartMs_ < config_.globalWindowMs) {
        wait = config_.globalWindowMs - (nowMs - windowStartMs_);
    }
    const int index = find(clientKey, nowMs);
    if (index >= 0 && slots_[index].lockedUntilMs > nowMs) {
        const uint64_t own = slots_[index].lockedUntilMs - nowMs;
        if (own > wait) wait = own;
    }
    return wait;
}

void LoginRateLimiter::recordFailure(uint64_t clientKey, uint64_t nowMs) {
    // A window opens at the first failure after the previous one closed.
    if (windowFailures_ == 0 || nowMs < windowStartMs_ ||
        nowMs - windowStartMs_ >= config_.globalWindowMs) {
        windowStartMs_ = nowMs;
        windowFailures_ = 0;
    }
    if (windowFailures_ < 0xFFFFFFFFu) windowFailures_++;

    int index = find(clientKey, nowMs);
    if (index < 0) {
        // Recycle a free/forgotten slot, else the one with the oldest failure.
        size_t target = 0;
        for (size_t i = 0; i < kSlots; i++) {
            const Slot &s = slots_[i];
            if (!s.used || nowMs < s.lastFailureMs || nowMs - s.lastFailureMs >= config_.forgetAfterMs) {
                target = i;
                break;
            }
            if (s.lastFailureMs < slots_[target].lastFailureMs) target = i;
        }
        slots_[target] = Slot{};
        slots_[target].used = true;
        slots_[target].key = clientKey;
        index = static_cast<int>(target);
    }
    Slot &slot = slots_[index];
    if (slot.failures < 0xFFFFFFFFu) slot.failures++;
    slot.lastFailureMs = nowMs;
    if (slot.failures > config_.freeFailures) {
        const uint32_t doublings = slot.failures - config_.freeFailures - 1;
        uint64_t lockout = config_.baseLockoutMs;
        for (uint32_t i = 0; i < doublings && lockout < config_.maxLockoutMs; i++) {
            lockout *= 2;
        }
        if (lockout > config_.maxLockoutMs) lockout = config_.maxLockoutMs;
        slot.lockedUntilMs = nowMs + lockout;
    }
}

void LoginRateLimiter::recordSuccess(uint64_t clientKey) {
    for (auto &slot : slots_) {
        if (slot.used && slot.key == clientKey) {
            slot = Slot{};
        }
    }
}

}  // namespace hubbo::auth
