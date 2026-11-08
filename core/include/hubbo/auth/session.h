#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hubbo/auth/crypto.h"

// Admin browser sessions, in RAM only.
//
// - Fixed capacity (kMaxSessions), no allocation after construction. When
//   full, creating a session evicts the least recently used one.
// - Session id and CSRF token: 32 random bytes each, base64url (43 chars).
//   The id goes in the `hubbo_session` cookie (HttpOnly, SameSite=Strict,
//   Secure under HTTPS); the CSRF token is returned in the login/session JSON
//   and must come back in `X-Hubbo-CSRF` on unsafe requests.
// - Idle timeout and absolute lifetime. Never persisted, so a reboot ends
//   every session.
// - Lookups compare against every slot in constant time.
//
// Times are a monotonic millisecond clock (uptime), injected by the caller.
namespace hubbo::auth {

constexpr size_t kMaxSessions = 8;
constexpr size_t kSessionSecretBytes = 32;
constexpr size_t kSessionSecretLength = base64UrlLength(kSessionSecretBytes);  // 43

struct SessionConfig {
    uint64_t idleTimeoutMs = 8ull * 60 * 60 * 1000;       // 8 h without use
    uint64_t absoluteTimeoutMs = 24ull * 60 * 60 * 1000;  // 24 h after login
};

struct NewSession {
    std::string id;    // cookie value
    std::string csrf;  // X-Hubbo-CSRF value
};

// Result of looking up a request's session cookie.
struct SessionAuth {
    bool valid = false;      // live session; its idle timer was refreshed
    bool csrfValid = false;  // `csrf` matched this session's CSRF token
    uint64_t expiresInMs = 0;  // until the earlier of idle and absolute expiry
};

class SessionStore {
public:
    explicit SessionStore(RandomSource &random, SessionConfig config = {});

    // Fails only if the random source fails.
    bool create(uint64_t nowMs, NewSession *out);

    // One lookup per request: is `id` a live session (refreshing its idle
    // timer), and is `csrf` (the X-Hubbo-CSRF header, empty if absent) its
    // CSRF token. Allocation-free.
    SessionAuth authenticate(std::string_view id, std::string_view csrf, uint64_t nowMs);

    // The CSRF token of a live session (for GET /auth/session), or empty.
    std::string csrfToken(std::string_view id, uint64_t nowMs) const;

    void remove(std::string_view id);
    // Every session except `keepId` (e.g. after a password change).
    void removeAllExcept(std::string_view keepId);
    void clear();

    size_t activeCount(uint64_t nowMs) const;

private:
    struct Slot {
        bool used = false;
        uint8_t id[kSessionSecretBytes] = {};
        uint8_t csrf[kSessionSecretBytes] = {};
        uint64_t createdMs = 0;
        uint64_t lastSeenMs = 0;
    };

    int find(std::string_view id, uint64_t nowMs) const;
    bool live(const Slot &slot, uint64_t nowMs) const;
    void wipe(Slot &slot);

    RandomSource &random_;
    SessionConfig config_;
    Slot slots_[kMaxSessions];
};

}  // namespace hubbo::auth
