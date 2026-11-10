#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hubbo/auth/crypto.h"

// Admin browser sessions, in RAM only.
//
// - Fixed capacity (kMaxSessions), no allocation after construction. When
//   full, creating a session evicts the oldest one.
// - Session id: 32 random bytes, base64url (43 chars), in the
//   `hubbo_session` cookie (HttpOnly, SameSite=Strict, Secure under HTTPS).
// - One absolute lifetime from login, no idle timer. Never persisted, so a
//   reboot (a firmware update too) ends every session.
// - Lookups compare against every slot in constant time.
//
// Times are a monotonic millisecond clock (uptime), injected by the caller.
namespace hubbo::auth {

constexpr size_t kMaxSessions = 8;
constexpr size_t kSessionSecretBytes = 32;
constexpr size_t kSessionSecretLength = base64UrlLength(kSessionSecretBytes);  // 43
constexpr uint64_t kSessionLifetimeMs = 7ull * 24 * 60 * 60 * 1000;           // 7 days after login

// Result of looking up a request's session cookie.
struct SessionAuth {
    bool valid = false;        // a live session
    uint64_t expiresInMs = 0;  // until it expires
};

class SessionStore {
public:
    explicit SessionStore(RandomSource &random, uint64_t lifetimeMs = kSessionLifetimeMs);

    // `*id` gets the cookie value. Fails only if the random source fails.
    bool create(uint64_t nowMs, std::string *id);

    // Is `id` a live session. Allocation-free.
    SessionAuth authenticate(std::string_view id, uint64_t nowMs) const;

    void remove(std::string_view id);
    // Every session except `keepId` (e.g. after a password change).
    void removeAllExcept(std::string_view keepId);
    void clear();

    size_t activeCount(uint64_t nowMs) const;

private:
    struct Slot {
        bool used = false;
        uint8_t id[kSessionSecretBytes] = {};
        uint64_t createdMs = 0;
    };

    int find(std::string_view id, uint64_t nowMs) const;
    bool live(const Slot &slot, uint64_t nowMs) const;
    void wipe(Slot &slot);

    RandomSource &random_;
    uint64_t lifetimeMs_;
    Slot slots_[kMaxSessions];
};

}  // namespace hubbo::auth
