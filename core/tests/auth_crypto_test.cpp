#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "hubbo/auth/crypto.h"
#include "hubbo/auth/password.h"
#include "auth_test_util.h"

using namespace hubbo::auth;
using hubbo::auth::test::CountingRandom;
using hubbo::auth::test::hex;

namespace {

std::string sha(const std::string &input) {
    SoftwareHash h;
    uint8_t out[kSha256Size];
    h.sha256(reinterpret_cast<const uint8_t *>(input.data()), input.size(), out);
    return hex(out, sizeof(out));
}

std::string hmac(const std::string &key, const std::string &data) {
    SoftwareHash h;
    uint8_t out[kSha256Size];
    h.hmacSha256(reinterpret_cast<const uint8_t *>(key.data()), key.size(),
                 reinterpret_cast<const uint8_t *>(data.data()), data.size(), out);
    return hex(out, sizeof(out));
}

std::string pbkdf2(Hash &h, const std::string &password, const std::string &salt, uint32_t c,
                   size_t length) {
    std::vector<uint8_t> out(length);
    h.pbkdf2Sha256(reinterpret_cast<const uint8_t *>(password.data()), password.size(),
                   reinterpret_cast<const uint8_t *>(salt.data()), salt.size(), c, out.data(),
                   length);
    return hex(out.data(), out.size());
}

// Uses only the base class's generic PBKDF2 (one hmacSha256() per iteration),
// the path an mbedTLS Hash that doesn't override pbkdf2Sha256 would take.
class GenericHash : public Hash {
public:
    void sha256(const uint8_t *d, size_t n, uint8_t out[kSha256Size]) override { inner.sha256(d, n, out); }
    void hmacSha256(const uint8_t *k, size_t kn, const uint8_t *d, size_t n,
                    uint8_t out[kSha256Size]) override {
        inner.hmacSha256(k, kn, d, n, out);
    }
    SoftwareHash inner;
};

}  // namespace

TEST(AuthCrypto, Sha256Vectors) {
    // FIPS 180-4 examples.
    EXPECT_EQ(sha(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(sha(std::string(1000000, 'a')),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(AuthCrypto, Sha256IncrementalMatchesOneShot) {
    const std::string text(200, 'x');
    for (size_t split = 0; split <= text.size(); split += 7) {
        Sha256 h;
        h.update(reinterpret_cast<const uint8_t *>(text.data()), split);
        h.update(reinterpret_cast<const uint8_t *>(text.data()) + split, text.size() - split);
        uint8_t out[kSha256Size];
        h.finish(out);
        EXPECT_EQ(hex(out, sizeof(out)), sha(text)) << split;
    }
}

TEST(AuthCrypto, HmacRfc4231) {
    EXPECT_EQ(hmac(std::string(20, '\x0b'), "Hi There"),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    EXPECT_EQ(hmac("Jefe", "what do ya want for nothing?"),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    EXPECT_EQ(hmac(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First"),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(AuthCrypto, Pbkdf2Vectors) {
    SoftwareHash fast;
    GenericHash generic;
    for (Hash *h : {static_cast<Hash *>(&fast), static_cast<Hash *>(&generic)}) {
        EXPECT_EQ(pbkdf2(*h, "password", "salt", 1, 32),
                  "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
        EXPECT_EQ(pbkdf2(*h, "password", "salt", 2, 32),
                  "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
        EXPECT_EQ(pbkdf2(*h, "password", "salt", 4096, 32),
                  "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
        // RFC 7914 section 11: multi-block output.
        EXPECT_EQ(pbkdf2(*h, "passwd", "salt", 1, 64),
                  "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                  "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
    }
}

TEST(AuthCrypto, ConstantTimeEqual) {
    const uint8_t a[4] = {1, 2, 3, 4};
    const uint8_t b[4] = {1, 2, 3, 5};
    EXPECT_TRUE(constantTimeEqual(a, a, 4));
    EXPECT_FALSE(constantTimeEqual(a, b, 4));
    EXPECT_TRUE(constantTimeEqual(a, b, 3));
}

TEST(AuthCrypto, Base64UrlRoundTripAndStrictness) {
    for (size_t n = 0; n < 40; n++) {
        std::string bytes;
        for (size_t i = 0; i < n; i++) bytes += static_cast<char>(i * 37 + 250);
        const std::string text =
            base64UrlEncode(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
        EXPECT_EQ(text.find('='), std::string::npos);
        std::string back;
        ASSERT_TRUE(base64UrlDecode(text, &back)) << text;
        EXPECT_EQ(back, bytes);
    }
    std::string out = "unchanged";
    EXPECT_FALSE(base64UrlDecode("YQ==", &out));  // padding
    EXPECT_FALSE(base64UrlDecode("Y Q", &out));
    EXPECT_FALSE(base64UrlDecode("a+b/", &out));  // standard alphabet
    EXPECT_FALSE(base64UrlDecode("Y", &out));     // impossible length
    EXPECT_FALSE(base64UrlDecode("YR", &out));    // non-zero trailing bits
    EXPECT_EQ(out, "unchanged");
    EXPECT_TRUE(base64UrlDecode("YQ", &out));
    EXPECT_EQ(out, "a");
}

TEST(AuthPassword, HashAndVerify) {
    SoftwareHash h;
    CountingRandom random;
    std::string stored;
    ASSERT_TRUE(hashPassword(h, random, "correct horse battery", kMinPasswordIterations, &stored));
    EXPECT_EQ(stored.rfind("pbkdf2-sha256$10000$", 0), 0u) << stored;
    EXPECT_EQ(verifyPassword(h, "correct horse battery", stored), PasswordCheck::Match);
    EXPECT_EQ(verifyPassword(h, "correct horse batterY", stored), PasswordCheck::Mismatch);
    EXPECT_EQ(verifyPassword(h, "", stored), PasswordCheck::Mismatch);

    std::string again;
    ASSERT_TRUE(hashPassword(h, random, "correct horse battery", kMinPasswordIterations, &again));
    EXPECT_NE(again, stored) << "salt must differ";
}

TEST(AuthPassword, RejectsWeakInputAndBadParameters) {
    SoftwareHash h;
    CountingRandom random;
    std::string out;
    EXPECT_FALSE(hashPassword(h, random, "short", kDefaultPasswordIterations, &out));
    EXPECT_FALSE(hashPassword(h, random, std::string(129, 'a'), kDefaultPasswordIterations, &out));
    EXPECT_FALSE(hashPassword(h, random, "long enough pw", 1000, &out));
    random.failing = true;
    EXPECT_FALSE(hashPassword(h, random, "long enough pw", kMinPasswordIterations, &out));
    EXPECT_TRUE(out.empty());
}

TEST(AuthPassword, MalformedStoredHashes) {
    SoftwareHash h;
    const char *bad[] = {
        "",
        "plaintext",
        "pbkdf2-sha1$60000$q1Vx0mJb3Yk8fT2nW6sR4g$Hn3pQ9vL2xZ7cK5rT8wM1bJ6yF4dG0sA9eU3iO7tX2c",
        "pbkdf2-sha256$100$q1Vx0mJb3Yk8fT2nW6sR4g$Hn3pQ9vL2xZ7cK5rT8wM1bJ6yF4dG0sA9eU3iO7tX2c",
        "pbkdf2-sha256$99999999999$q1Vx0mJb3Yk8fT2nW6sR4g$Hn3pQ9vL2xZ7cK5rT8wM1bJ6yF4dG0sA9eU3iO7tX2c",
        "pbkdf2-sha256$060000$q1Vx0mJb3Yk8fT2nW6sR4g$Hn3pQ9vL2xZ7cK5rT8wM1bJ6yF4dG0sA9eU3iO7tX2c",
        "pbkdf2-sha256$60000$short$Hn3pQ9vL2xZ7cK5rT8wM1bJ6yF4dG0sA9eU3iO7tX2c",
        "pbkdf2-sha256$60000$q1Vx0mJb3Yk8fT2nW6sR4g$Hn3pQ9vL2x",
        "pbkdf2-sha256$60000$q1Vx0mJb3Yk8fT2nW6sR4g$Hn3pQ9vL2xZ7cK5rT8wM1bJ6yF4dG0sA9eU3iO7tX2c$x",
    };
    for (const char *stored : bad) {
        EXPECT_EQ(verifyPassword(h, "whatever pw", stored), PasswordCheck::Malformed) << stored;
    }
}

TEST(AuthPassword, DummyHashIsWellFormedAndMatchesNothingObvious) {
    SoftwareHash h;
    EXPECT_EQ(verifyPassword(h, "", dummyPasswordHash()), PasswordCheck::Mismatch);
    EXPECT_EQ(verifyPassword(h, "admin", dummyPasswordHash()), PasswordCheck::Mismatch);
}
