#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hubbo/auth/crypto.h"
#include "hubbo/auth/password.h"
#include "hubbo/auth/secret_store.h"
#include "hubbo/auth/token.h"

// The admin account and API tokens, persisted in a SecretStore.
//
// Token hashes are cached in RAM (load() at boot) so a bearer request costs
// one SHA-256 and no flash read. `last_used_at` is RAM only: it resets on
// reboot rather than wearing flash on every API call.
//
// Times are wall-clock Unix seconds, or -1 when the hub has no time yet
// (the API then reports null).
namespace hubbo::auth {

enum class CredentialResult : uint8_t {
    Ok,
    AlreadyExists,    // setup after an admin exists
    NoAdmin,          // login/password change before setup
    InvalidUsername,  // ^[A-Za-z0-9_.-]{1,32}$
    WeakPassword,     // see isAcceptablePassword()
    WrongPassword,
    InvalidName,      // token name: 1..64 bytes, no control characters
    LimitReached,     // kMaxApiTokens
    NotFound,
    StorageFailed,    // SecretStore write failed; nothing changed
    NoEntropy,        // RandomSource failed
    Unavailable,      // secrets couldn't be read: unknown, so nothing may be decided or written
    Corrupt,          // secrets exist but can't be decoded
};

// What load() found. "Can't read" is never "doesn't exist": only Fresh lets
// anyone create the admin account.
enum class CredentialState : uint8_t {
    Fresh,        // positively no admin (store readable, admin not found)
    Ok,           // admin (and tokens) loaded
    Unavailable,  // the store couldn't be read; refresh() retries
    Corrupt,      // secrets exist but can't be decoded (or tokens without an admin)
};
const char *credentialStateName(CredentialState state);  // "fresh", "ok", "unavailable", "corrupt"

constexpr size_t kMaxUsernameLength = 32;
bool isValidUsername(std::string_view username);  // ^[A-Za-z0-9_.-]{1,32}$

class Credentials {
public:
    Credentials(SecretStore &store, Hash &hash, RandomSource &random,
                uint32_t passwordIterations = kDefaultPasswordIterations);

    // Reads the admin and token records and sets state(). Call once at boot.
    // A malformed token record is skipped (that token just doesn't work).
    void load();
    // Unless Ok with a readable store: reload the store from its medium and
    // load() again. So credentials follow the medium both ways: when it goes
    // down they become Unavailable (nothing cached is trusted), when it comes
    // back they reload. The authenticator calls it on every request with a
    // monotonic clock. Outside Ok the reload backs off like
    // ConfigService::refresh(): at once on a medium health transition, then
    // 1 s doubling to 60 s while the state stays the same. setupAdmin()
    // doesn't use the schedule: it always re-reads before deciding.
    void refresh(uint64_t nowMs);
    static constexpr uint64_t kFirstRetryMs = 1000;
    static constexpr uint64_t kMaxRetryMs = 60000;

    CredentialState state() const { return state_; }
    bool adminExists() const { return state_ == CredentialState::Ok; }
    const std::string &username() const { return username_; }

    // Only in Fresh, re-checked against storage first (always, whatever the
    // refresh() schedule): AlreadyExists (Ok),
    // Unavailable or Corrupt otherwise. One write ("admin"), so a crash can't
    // leave half an account.
    CredentialResult setupAdmin(std::string_view username, std::string_view password);

    // Same cost for an unknown username as for a wrong password. False unless Ok.
    bool verifyLogin(std::string_view username, std::string_view password);

    CredentialResult changePassword(std::string_view current, std::string_view next);

    const std::vector<StoredToken> &tokens() const { return tokens_; }
    // `issued->plaintext` is the only copy of the secret: return it once.
    CredentialResult createToken(std::string_view name, int64_t nowUnix, IssuedToken *issued);
    CredentialResult revokeToken(std::string_view id);
    // The token if `presented` is a live token (updates last-used).
    const StoredToken *authenticateToken(std::string_view presented, int64_t nowUnix);

private:
    static std::string tokenKey(std::string_view id);
    bool hasTokenId(std::string_view id) const;

    SecretStore &store_;
    Hash &hash_;
    RandomSource &random_;
    uint32_t iterations_;
    CredentialResult writeGuard() const;  // Ok, or why credentials can't change now
    void reread();                        // store reload + load(), no schedule

    // refresh() backoff (see above).
    uint64_t retryDelayMs_ = 0;  // 0: reload at the next refresh
    uint64_t retryAtMs_ = 0;
    bool retryHealthKnown_ = false;
    storage::Health retryHealth_ = storage::Health::Available;

    CredentialState state_ = CredentialState::Unavailable;
    std::string username_;
    std::string passwordHash_;
    std::vector<StoredToken> tokens_;
};

}  // namespace hubbo::auth
