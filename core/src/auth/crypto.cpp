#include "hubbo/auth/crypto.h"

#include <cstring>

namespace hubbo::auth {

namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int alphabetIndex(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

// HMAC key schedule: the inner and outer hash states after absorbing the
// padded key, so each message costs only its own blocks.
struct HmacState {
    Sha256 inner;
    Sha256 outer;

    HmacState(const uint8_t *key, size_t keyLength) {
        uint8_t block[64] = {};
        if (keyLength > sizeof(block)) {
            Sha256 h;
            h.update(key, keyLength);
            h.finish(block);
        } else if (keyLength > 0) {
            std::memcpy(block, key, keyLength);
        }
        uint8_t pad[64];
        for (size_t i = 0; i < 64; i++) pad[i] = block[i] ^ 0x36;
        inner.update(pad, 64);
        for (size_t i = 0; i < 64; i++) pad[i] = block[i] ^ 0x5c;
        outer.update(pad, 64);
        secureZero(block, sizeof(block));
        secureZero(pad, sizeof(pad));
    }

    // HMAC(key, a || b); b may be empty.
    void mac(const uint8_t *a, size_t aLength, const uint8_t *b, size_t bLength,
             uint8_t out[kSha256Size]) const {
        Sha256 i = inner;
        i.update(a, aLength);
        if (bLength > 0) i.update(b, bLength);
        uint8_t digest[kSha256Size];
        i.finish(digest);
        Sha256 o = outer;
        o.update(digest, sizeof(digest));
        o.finish(out);
    }
};

}  // namespace

void Sha256::reset() {
    static constexpr uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(state_, kInit, sizeof(state_));
    totalBytes_ = 0;
    buffered_ = 0;
}

void Sha256::block(const uint8_t *chunk) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t(chunk[i * 4]) << 24) | (uint32_t(chunk[i * 4 + 1]) << 16) |
               (uint32_t(chunk[i * 4 + 2]) << 8) | uint32_t(chunk[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + ch + kRoundConstants[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const uint8_t *data, size_t length) {
    totalBytes_ += length;
    while (length > 0) {
        if (buffered_ == 0 && length >= 64) {
            block(data);
            data += 64;
            length -= 64;
            continue;
        }
        const size_t take = (64 - buffered_ < length) ? 64 - buffered_ : length;
        std::memcpy(buffer_ + buffered_, data, take);
        buffered_ += take;
        data += take;
        length -= take;
        if (buffered_ == 64) {
            block(buffer_);
            buffered_ = 0;
        }
    }
}

void Sha256::finish(uint8_t out[kSha256Size]) {
    const uint64_t bits = totalBytes_ * 8;
    buffer_[buffered_++] = 0x80;
    if (buffered_ > 56) {
        std::memset(buffer_ + buffered_, 0, 64 - buffered_);
        block(buffer_);
        buffered_ = 0;
    }
    std::memset(buffer_ + buffered_, 0, 56 - buffered_);
    for (int i = 0; i < 8; i++) {
        buffer_[56 + i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    }
    block(buffer_);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = static_cast<uint8_t>(state_[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
    }
    reset();
}

namespace {

// RFC 8018 PBKDF2 over a PRF given as prf(a, aLength, b, bLength, out) =
// HMAC(password, a || b). Both Hash implementations share this loop.
template <typename Prf>
void pbkdf2(Prf prf, const uint8_t *salt, size_t saltLength, uint32_t iterations, uint8_t *out,
            size_t outLength) {
    uint8_t u[kSha256Size];
    uint8_t t[kSha256Size];
    for (uint32_t blockIndex = 1; outLength > 0; blockIndex++) {
        const uint8_t counter[4] = {static_cast<uint8_t>(blockIndex >> 24),
                                    static_cast<uint8_t>(blockIndex >> 16),
                                    static_cast<uint8_t>(blockIndex >> 8),
                                    static_cast<uint8_t>(blockIndex)};
        prf(salt, saltLength, counter, sizeof(counter), u);
        std::memcpy(t, u, sizeof(t));
        for (uint32_t i = 1; i < iterations; i++) {
            uint8_t next[kSha256Size];  // implementations need not support aliasing
            prf(u, sizeof(u), nullptr, 0, next);
            std::memcpy(u, next, sizeof(u));
            for (size_t j = 0; j < sizeof(t); j++) t[j] ^= u[j];
        }
        const size_t take = outLength < sizeof(t) ? outLength : sizeof(t);
        std::memcpy(out, t, take);
        out += take;
        outLength -= take;
    }
    secureZero(u, sizeof(u));
    secureZero(t, sizeof(t));
}

}  // namespace

void Hash::pbkdf2Sha256(const uint8_t *password, size_t passwordLength, const uint8_t *salt,
                        size_t saltLength, uint32_t iterations, uint8_t *out, size_t outLength) {
    // Generic fallback: one hmacSha256() call per iteration.
    std::string message;
    auto prf = [&](const uint8_t *a, size_t an, const uint8_t *b, size_t bn, uint8_t *mac) {
        if (bn == 0) {
            hmacSha256(password, passwordLength, a, an, mac);
            return;
        }
        message.assign(reinterpret_cast<const char *>(a), an);
        message.append(reinterpret_cast<const char *>(b), bn);
        hmacSha256(password, passwordLength, reinterpret_cast<const uint8_t *>(message.data()),
                   message.size(), mac);
    };
    pbkdf2(prf, salt, saltLength, iterations, out, outLength);
}

void SoftwareHash::sha256(const uint8_t *data, size_t length, uint8_t out[kSha256Size]) {
    Sha256 h;
    h.update(data, length);
    h.finish(out);
}

void SoftwareHash::hmacSha256(const uint8_t *key, size_t keyLength, const uint8_t *data,
                              size_t length, uint8_t out[kSha256Size]) {
    HmacState(key, keyLength).mac(data, length, nullptr, 0, out);
}

void SoftwareHash::pbkdf2Sha256(const uint8_t *password, size_t passwordLength,
                                const uint8_t *salt, size_t saltLength, uint32_t iterations,
                                uint8_t *out, size_t outLength) {
    const HmacState state(password, passwordLength);
    auto prf = [&](const uint8_t *a, size_t an, const uint8_t *b, size_t bn, uint8_t *mac) {
        state.mac(a, an, b, bn, mac);
    };
    pbkdf2(prf, salt, saltLength, iterations, out, outLength);
}

bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t length) {
    uint8_t diff = 0;
    for (size_t i = 0; i < length; i++) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

void secureZero(void *data, size_t length) {
    volatile uint8_t *p = static_cast<volatile uint8_t *>(data);
    while (length-- > 0) {
        *p++ = 0;
    }
}

std::string base64UrlEncode(const uint8_t *data, size_t length) {
    std::string out;
    out.reserve(base64UrlLength(length));
    size_t i = 0;
    for (; i + 3 <= length; i += 3) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (length - i == 1) {
        const uint32_t v = uint32_t(data[i]) << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
    } else if (length - i == 2) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
    }
    return out;
}

bool base64UrlDecode(std::string_view text, uint8_t *out, size_t capacity, size_t *length) {
    if (text.size() % 4 == 1 || base64UrlDecodedLength(text.size()) > capacity) {
        return false;
    }
    size_t written = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        const int v = alphabetIndex(c);
        if (v < 0) {
            return false;
        }
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[written++] = static_cast<uint8_t>(acc >> bits);
        }
    }
    // Leftover bits must be zero, so each byte string has one encoding.
    if (bits > 0 && (acc & ((1u << bits) - 1)) != 0) {
        return false;
    }
    *length = written;
    return true;
}

bool base64UrlDecode(std::string_view text, std::string *out) {
    std::string result(base64UrlDecodedLength(text.size()), '\0');
    size_t length = 0;
    if (!base64UrlDecode(text, reinterpret_cast<uint8_t *>(&result[0]), result.size(), &length)) {
        return false;
    }
    result.resize(length);
    *out = std::move(result);
    return true;
}

bool isBase64UrlChar(char c) { return alphabetIndex(c) >= 0; }

bool asciiEqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

}  // namespace hubbo::auth
