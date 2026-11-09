#include "hubbo/auth/password.h"

#include <cstring>
#include <string>

namespace hubbo::auth {

namespace {

constexpr char kPrefix[] = "pbkdf2-sha256$";
constexpr size_t kSaltSize = 16;
constexpr size_t kKeySize = 32;

bool parseIterations(std::string_view text, uint32_t *out) {
    if (text.empty() || text.size() > 10 || text[0] == '0') {
        return false;
    }
    uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    if (value > 0xFFFFFFFFu) {
        return false;
    }
    *out = static_cast<uint32_t>(value);
    return true;
}

}  // namespace

bool isAcceptablePassword(std::string_view password) {
    return password.size() >= kMinPasswordLength && password.size() <= kMaxPasswordLength;
}

bool hashPassword(Hash &hash, RandomSource &random, std::string_view password,
                  uint32_t iterations, std::string *encoded) {
    if (!isAcceptablePassword(password) || iterations < kMinPasswordIterations ||
        iterations > kMaxPasswordIterations) {
        return false;
    }
    uint8_t salt[kSaltSize];
    if (!random.fill(salt, sizeof(salt))) {
        return false;
    }
    uint8_t key[kKeySize];
    hash.pbkdf2Sha256(reinterpret_cast<const uint8_t *>(password.data()), password.size(), salt,
                      sizeof(salt), iterations, key, sizeof(key));
    std::string result = kPrefix;
    result += std::to_string(iterations);
    result += '$';
    result += base64UrlEncode(salt, sizeof(salt));
    result += '$';
    result += base64UrlEncode(key, sizeof(key));
    secureZero(key, sizeof(key));
    *encoded = std::move(result);
    return true;
}

PasswordCheck verifyPassword(Hash &hash, std::string_view password, std::string_view encoded) {
    const std::string_view prefix(kPrefix);
    if (encoded.substr(0, prefix.size()) != prefix) {
        return PasswordCheck::Malformed;
    }
    std::string_view rest = encoded.substr(prefix.size());
    const size_t first = rest.find('$');
    if (first == std::string_view::npos) {
        return PasswordCheck::Malformed;
    }
    const size_t second = rest.find('$', first + 1);
    if (second == std::string_view::npos || rest.find('$', second + 1) != std::string_view::npos) {
        return PasswordCheck::Malformed;
    }
    uint32_t iterations = 0;
    uint8_t salt[kSaltSize];
    uint8_t expected[kKeySize];
    size_t saltLength = 0;
    size_t keyLength = 0;
    if (!parseIterations(rest.substr(0, first), &iterations) ||
        iterations < kMinPasswordIterations || iterations > kMaxPasswordIterations ||
        !base64UrlDecode(rest.substr(first + 1, second - first - 1), salt, sizeof(salt),
                         &saltLength) ||
        saltLength != kSaltSize ||
        !base64UrlDecode(rest.substr(second + 1), expected, sizeof(expected), &keyLength) ||
        keyLength != kKeySize) {
        return PasswordCheck::Malformed;
    }
    // Over-long input still costs a derivation, then fails.
    const bool tooLong = password.size() > kMaxPasswordLength;
    const std::string_view used = tooLong ? password.substr(0, kMaxPasswordLength) : password;
    uint8_t key[kKeySize];
    hash.pbkdf2Sha256(reinterpret_cast<const uint8_t *>(used.data()), used.size(),
                      salt, sizeof(salt), iterations, key, sizeof(key));
    const bool same = constantTimeEqual(key, expected, kKeySize);
    secureZero(key, sizeof(key));
    return (same && !tooLong) ? PasswordCheck::Match : PasswordCheck::Mismatch;
}

const char *dummyPasswordHash() {
    // Random salt and key with no known preimage, at the default count so an
    // unknown username costs as much as a wrong password.
    static const std::string dummy = kPrefix + std::to_string(kDefaultPasswordIterations) +
                                     "$q1Vx0mJb3Yk8fT2nW6sR4g$Hn3pQ9vL2xZ7cK5rT8wM1bJ6yF4dG0sA9eU3iO7tX2c";
    return dummy.c_str();
}

}  // namespace hubbo::auth
