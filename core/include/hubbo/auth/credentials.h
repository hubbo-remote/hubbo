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
    Unavailable,      // secrets couldn't be read or decoded at boot: nothing may be decided or written
};

// What load() found, once at boot. "Can't read" is never "doesn't exist":
// only Fresh lets anyone create the admin account. Recovery from
// Unavailable is a reboot (or the wipe).
enum class CredentialState : uint8_t {
    Fresh,        // positively no admin (store readable, admin not found)
    Ok,           // admin (and tokens) loaded
    Unavailable,  // the store couldn't be read, or holds damaged credentials (or tokens without an admin)
};
const char *credentialStateName(CredentialState state);  // "fresh", "ok", "unavailable"

constexpr size_t kMaxUsernameLength = 32;
bool isValidUsername(std::string_view username);  // ^[A-Za-z0-9_.-]{1,32}$

class Credentials {
public:
    Credentials(SecretStore &store, Hash &hash, RandomSource &random,
                uint32_t passwordIterations = kDefaultPasswordIterations);

    // Reads the admin and token records and sets state(). Call once at boot.
    // A malformed token record is skipped (that token just doesn't work).
    void load();

    CredentialState state() const { return state_; }
    // Ok, or why the account can't be used now (login, password, tokens):
    // NoAdmin (Fresh) or Unavailable.
    CredentialResult adminGuard() const;
    // Ok while Fresh, or why setup is refused: AlreadyExists or Unavailable.
    CredentialResult setupGuard() const;
    const std::string &username() const { return username_; }

    // Only in Fresh: AlreadyExists (Ok), Unavailable otherwise. The caller
    // picks the username (no default). One write ("admin"), so a crash can't
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

    CredentialState state_ = CredentialState::Unavailable;
    std::string username_;
    std::string passwordHash_;
    std::vector<StoredToken> tokens_;
};

}  // namespace hubbo::auth
