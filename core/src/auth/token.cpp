#include "hubbo/auth/token.h"

namespace hubbo::auth {

namespace {

constexpr char kIdAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";  // 36

bool isIdChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

}  // namespace

bool issueToken(Hash &hash, RandomSource &random, IssuedToken *out) {
    std::string id;
    // Rejection sampling keeps the id uniform over 36 symbols (252 = 7*36).
    while (id.size() < kTokenIdLength) {
        uint8_t byte[8];
        if (!random.fill(byte, sizeof(byte))) {
            return false;
        }
        for (uint8_t b : byte) {
            if (b < 252 && id.size() < kTokenIdLength) {
                id += kIdAlphabet[b % 36];
            }
        }
    }
    uint8_t secret[kTokenSecretBytes];
    if (!random.fill(secret, sizeof(secret))) {
        return false;
    }
    std::string plaintext = "hbt_" + id + "_" + base64UrlEncode(secret, sizeof(secret));
    secureZero(secret, sizeof(secret));
    hash.sha256(reinterpret_cast<const uint8_t *>(plaintext.data()), plaintext.size(), out->hash);
    out->id = std::move(id);
    out->plaintext = std::move(plaintext);
    return true;
}

bool parseToken(std::string_view token, std::string_view *id) {
    if (token.size() != kTokenLength || token.substr(0, 4) != "hbt_" ||
        token[4 + kTokenIdLength] != '_') {
        return false;
    }
    const std::string_view idPart = token.substr(4, kTokenIdLength);
    for (char c : idPart) {
        if (!isIdChar(c)) return false;
    }
    for (char c : token.substr(5 + kTokenIdLength)) {
        if (!isBase64UrlChar(c)) return false;
    }
    if (id != nullptr) {
        *id = idPart;
    }
    return true;
}

const StoredToken *findToken(Hash &hash, std::string_view presented, const StoredToken *tokens,
                             size_t count) {
    if (!parseToken(presented, nullptr)) {
        return nullptr;
    }
    uint8_t digest[kSha256Size];
    hash.sha256(reinterpret_cast<const uint8_t *>(presented.data()), presented.size(), digest);
    const StoredToken *match = nullptr;
    for (size_t i = 0; i < count; i++) {
        if (constantTimeEqual(digest, tokens[i].hash, kSha256Size) && match == nullptr) {
            match = &tokens[i];
        }
    }
    return match;
}

std::string_view bearerToken(std::string_view header) {
    if (header.size() < 8 || header[6] != ' ' ||
        !asciiEqualsIgnoreCase(header.substr(0, 6), "bearer")) {
        return {};
    }
    std::string_view token = header.substr(7);
    if (token.find(' ') != std::string_view::npos) {
        return {};
    }
    return token;
}

}  // namespace hubbo::auth
