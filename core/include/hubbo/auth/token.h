#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hubbo/auth/crypto.h"

// API tokens.
//
//   hbt_<id>_<secret>
//   id      8 chars [a-z0-9], public (shown in the token list as tok_<id>)
//   secret  32 random bytes, base64url (43 chars)
//
// The hub stores only {id, name, SHA-256(whole token)}. A fast hash is fine
// because the token has 256 bits of entropy (no dictionary to attack); a slow
// KDF would only make every API request slow. The plaintext is returned once
// at creation and never again.
namespace hubbo::auth {

constexpr size_t kTokenIdLength = 8;
constexpr size_t kTokenSecretBytes = 32;
constexpr size_t kTokenSecretLength = base64UrlLength(kTokenSecretBytes);  // 43
constexpr size_t kTokenLength = 4 + kTokenIdLength + 1 + kTokenSecretLength;  // 56
constexpr size_t kMaxApiTokens = 16;

struct IssuedToken {
    std::string id;         // "k3x9q2mf"
    std::string plaintext;  // "hbt_k3x9q2mf_..." (show once, then discard)
    uint8_t hash[kSha256Size];
};

// Fails (false) only if the random source fails.
bool issueToken(Hash &hash, RandomSource &random, IssuedToken *out);

// Shape check only. On success sets *id to the token's id part.
bool parseToken(std::string_view token, std::string_view *id);

struct StoredToken {
    std::string id;
    std::string name;
    uint8_t hash[kSha256Size];
};

// The stored token matching `presented`, or nullptr. Hashes the presented
// token once and compares against every stored hash in constant time (no
// early exit on id), so timing reveals neither which ids exist nor how much
// of a secret matched.
const StoredToken *findToken(Hash &hash, std::string_view presented, const StoredToken *tokens,
                             size_t count);

// Extracts the token from an `Authorization` header value: "Bearer <token>"
// (scheme case-insensitive, one space). Empty view if absent/malformed.
std::string_view bearerToken(std::string_view authorizationHeader);

}  // namespace hubbo::auth
