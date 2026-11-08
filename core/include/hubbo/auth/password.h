#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hubbo/auth/crypto.h"

// Admin password hashing: PBKDF2-HMAC-SHA256, 16-byte random salt, 32-byte
// key, stored as one self-describing string so the iteration count can be
// raised later without a migration:
//
//   pbkdf2-sha256$<iterations>$<salt base64url>$<key base64url>
//
// The stored string goes in NVS (see docs/security.md), never in exported
// JSON or backups.
namespace hubbo::auth {

// Default work factor. PBKDF2 is the only option mbedTLS gives us without
// extra code; this is a compromise for a ~240 MHz MCU (measure login latency
// on hardware and raise it if it's well under ~0.5 s). Login is also rate
// limited, which matters more on a LAN appliance.
constexpr uint32_t kDefaultPasswordIterations = 60000;
// Accepted range when verifying a stored hash. Bounds the work an attacker
// who can write NVS could force, and rejects absurdly weak hashes.
constexpr uint32_t kMinPasswordIterations = 10000;
constexpr uint32_t kMaxPasswordIterations = 2000000;

constexpr size_t kMinPasswordLength = 10;
constexpr size_t kMaxPasswordLength = 128;

// Length only (bytes): 10..128. No composition rules (NIST SP 800-63B).
bool isAcceptablePassword(std::string_view password);

// Returns false if the password is unacceptable, iterations are out of range,
// or the random source fails.
bool hashPassword(Hash &hash, RandomSource &random, std::string_view password,
                  uint32_t iterations, std::string *encoded);

enum class PasswordCheck : uint8_t { Match, Mismatch, Malformed };

// Constant-time comparison of the derived key. Callers should still run a
// verification against a dummy hash for an unknown username, so a wrong
// username costs the same time as a wrong password (see
// dummyPasswordHash()).
PasswordCheck verifyPassword(Hash &hash, std::string_view password, std::string_view encoded);

// A valid hash string of an unguessable password with the default work
// factor, for equal-time handling of unknown usernames. Same value each call.
const char *dummyPasswordHash();

}  // namespace hubbo::auth
