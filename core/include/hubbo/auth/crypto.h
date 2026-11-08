#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Crypto primitives the auth code needs, behind injected interfaces.
//
//   Hash          SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256. SoftwareHash is a
//                 portable implementation (used natively and fine on the ESP32);
//                 the firmware may override with mbedTLS for hardware SHA.
//   RandomSource  cryptographically secure random bytes. Platform-provided:
//                 esp_fill_random() on the ESP32 (with RF or the bootloader
//                 entropy source enabled), getrandom() natively. Tests use a
//                 deterministic double. There is deliberately no default.
namespace hubbo::auth {

constexpr size_t kSha256Size = 32;

class Hash {
public:
    virtual ~Hash() = default;
    virtual void sha256(const uint8_t *data, size_t length, uint8_t out[kSha256Size]) = 0;
    virtual void hmacSha256(const uint8_t *key, size_t keyLength, const uint8_t *data,
                            size_t length, uint8_t out[kSha256Size]) = 0;
    // RFC 8018 PBKDF2 with HMAC-SHA256. The default derives it from
    // hmacSha256(); implementations may override with a faster one.
    virtual void pbkdf2Sha256(const uint8_t *password, size_t passwordLength, const uint8_t *salt,
                              size_t saltLength, uint32_t iterations, uint8_t *out,
                              size_t outLength);
};

class RandomSource {
public:
    virtual ~RandomSource() = default;
    // Fill `out` with secure random bytes. Return false if no entropy is
    // available; callers then fail closed.
    virtual bool fill(uint8_t *out, size_t length) = 0;
};

// Portable SHA-256 (FIPS 180-4).
class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const uint8_t *data, size_t length);
    void finish(uint8_t out[kSha256Size]);

private:
    void block(const uint8_t *chunk);
    uint32_t state_[8];
    uint64_t totalBytes_;
    uint8_t buffer_[64];
    size_t buffered_;
};

class SoftwareHash : public Hash {
public:
    void sha256(const uint8_t *data, size_t length, uint8_t out[kSha256Size]) override;
    void hmacSha256(const uint8_t *key, size_t keyLength, const uint8_t *data, size_t length,
                    uint8_t out[kSha256Size]) override;
    // Precomputes the HMAC pads once: about 2 compressions per iteration.
    void pbkdf2Sha256(const uint8_t *password, size_t passwordLength, const uint8_t *salt,
                      size_t saltLength, uint32_t iterations, uint8_t *out,
                      size_t outLength) override;
};

// Compares in time that depends only on `length`.
bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t length);

// Overwrite memory the compiler may not optimise away.
void secureZero(void *data, size_t length);

// base64url without padding (RFC 4648 section 5).
constexpr size_t base64UrlLength(size_t bytes) { return (bytes * 4 + 2) / 3; }
constexpr size_t base64UrlDecodedLength(size_t chars) { return chars * 3 / 4; }
std::string base64UrlEncode(const uint8_t *data, size_t length);
// Strict: rejects padding, whitespace and any non-alphabet byte, and
// non-canonical trailing bits. Returns false on error (`*out`/`*length`
// unchanged; the buffer form may have written into `out`). The buffer form
// needs no allocation and fails if `capacity` is too small.
bool base64UrlDecode(std::string_view text, uint8_t *out, size_t capacity, size_t *length);
bool base64UrlDecode(std::string_view text, std::string *out);
bool isBase64UrlChar(char c);

// ASCII-only case-insensitive comparison (HTTP header names and schemes).
bool asciiEqualsIgnoreCase(std::string_view a, std::string_view b);

}  // namespace hubbo::auth
