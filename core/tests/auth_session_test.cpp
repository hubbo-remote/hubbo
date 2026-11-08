#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "auth_test_util.h"
#include "hubbo/auth/rate_limit.h"
#include "hubbo/auth/session.h"
#include "hubbo/auth/token.h"

using namespace hubbo::auth;
using hubbo::auth::test::CountingRandom;

namespace {
constexpr uint64_t kHour = 60ull * 60 * 1000;
}

// ---------------------------------------------------------------- tokens

TEST(AuthToken, IssueParseAndFind) {
    SoftwareHash h;
    CountingRandom random;
    std::vector<StoredToken> stored;
    std::vector<std::string> plaintexts;
    std::set<std::string> ids;
    for (int i = 0; i < 5; i++) {
        IssuedToken t;
        ASSERT_TRUE(issueToken(h, random, &t));
        EXPECT_EQ(t.plaintext.size(), kTokenLength);
        EXPECT_EQ(t.plaintext.rfind("hbt_" + t.id + "_", 0), 0u);
        std::string_view id;
        ASSERT_TRUE(parseToken(t.plaintext, &id));
        EXPECT_EQ(id, t.id);
        EXPECT_TRUE(ids.insert(t.id).second);
        StoredToken s;
        s.id = t.id;
        s.name = "token " + std::to_string(i);
        std::copy(t.hash, t.hash + kSha256Size, s.hash);
        stored.push_back(s);
        plaintexts.push_back(t.plaintext);
    }
    for (size_t i = 0; i < plaintexts.size(); i++) {
        const StoredToken *found = findToken(h, plaintexts[i], stored.data(), stored.size());
        ASSERT_NE(found, nullptr);
        EXPECT_EQ(found->id, stored[i].id);
    }
    // Revoked (removed from the store) -> gone.
    EXPECT_EQ(findToken(h, plaintexts[0], stored.data() + 1, stored.size() - 1), nullptr);
}

TEST(AuthToken, RejectsWrongOrMalformed) {
    SoftwareHash h;
    CountingRandom random;
    IssuedToken t;
    ASSERT_TRUE(issueToken(h, random, &t));
    StoredToken s;
    s.id = t.id;
    std::copy(t.hash, t.hash + kSha256Size, s.hash);

    std::string flipped = t.plaintext;
    flipped.back() = flipped.back() == 'A' ? 'B' : 'A';
    std::string otherId = t.plaintext;
    otherId[4] = otherId[4] == 'a' ? 'b' : 'a';
    const std::string cases[] = {
        flipped, otherId, "", "hbt_", t.plaintext + "x", t.plaintext.substr(1),
        "Bearer " + t.plaintext, "hbt_ABCDEFGH_" + t.plaintext.substr(13),
        std::string(kTokenLength, 'a'),
    };
    for (const auto &c : cases) {
        EXPECT_EQ(findToken(h, c, &s, 1), nullptr) << c;
    }
}

TEST(AuthToken, BearerHeader) {
    EXPECT_EQ(bearerToken("Bearer abc"), "abc");
    EXPECT_EQ(bearerToken("bearer abc"), "abc");
    EXPECT_EQ(bearerToken("Basic abc"), "");
    EXPECT_EQ(bearerToken("Bearer"), "");
    EXPECT_EQ(bearerToken("Bearer  abc"), "");
    EXPECT_EQ(bearerToken("Bearer a b"), "");
    EXPECT_EQ(bearerToken("Bearerabc"), "");
}

TEST(AuthToken, FailsClosedWithoutEntropy) {
    SoftwareHash h;
    CountingRandom random;
    random.failing = true;
    IssuedToken t;
    EXPECT_FALSE(issueToken(h, random, &t));
}

// ---------------------------------------------------------------- sessions

TEST(AuthSession, CreateTouchCsrfLogout) {
    CountingRandom random;
    SessionStore store(random);
    NewSession s;
    ASSERT_TRUE(store.create(1000, &s));
    EXPECT_EQ(s.id.size(), kSessionSecretLength);
    EXPECT_EQ(s.csrf.size(), kSessionSecretLength);
    EXPECT_NE(s.id, s.csrf);

    const SessionAuth ok = store.authenticate(s.id, s.csrf, 2000);
    EXPECT_TRUE(ok.valid);
    EXPECT_TRUE(ok.csrfValid);
    EXPECT_EQ(ok.expiresInMs, SessionConfig{}.idleTimeoutMs);
    EXPECT_TRUE(store.authenticate(s.id, "", 2000).valid) << "GET without CSRF header";
    EXPECT_FALSE(store.authenticate(s.id, "", 2000).csrfValid);
    EXPECT_FALSE(store.authenticate(s.id, s.id, 2000).csrfValid);
    EXPECT_EQ(store.csrfToken(s.id, 2000), s.csrf);

    store.remove(s.id);
    const SessionAuth gone = store.authenticate(s.id, s.csrf, 3000);
    EXPECT_FALSE(gone.valid);
    EXPECT_FALSE(gone.csrfValid);
    EXPECT_EQ(store.csrfToken(s.id, 3000), "");
}

TEST(AuthSession, CsrfTokenOfAnotherSessionIsRejected) {
    CountingRandom random;
    SessionStore store(random);
    NewSession a, b;
    ASSERT_TRUE(store.create(0, &a));
    ASSERT_TRUE(store.create(0, &b));
    EXPECT_FALSE(store.authenticate(a.id, b.csrf, 1).csrfValid);
    EXPECT_FALSE(store.authenticate(b.id, a.csrf, 1).csrfValid);
}

TEST(AuthSession, IdleAndAbsoluteExpiry) {
    CountingRandom random;
    SessionStore store(random, SessionConfig{2 * kHour, 5 * kHour});
    NewSession s;
    ASSERT_TRUE(store.create(0, &s));
    // Kept alive by use every hour...
    for (uint64_t t = kHour; t < 5 * kHour; t += kHour) {
        EXPECT_TRUE(store.authenticate(s.id, "", t).valid) << t;
    }
    // ...but not past the absolute lifetime.
    EXPECT_FALSE(store.authenticate(s.id, "", 5 * kHour).valid);

    NewSession idle;
    ASSERT_TRUE(store.create(10 * kHour, &idle));
    EXPECT_TRUE(store.authenticate(idle.id, "", 10 * kHour + 2 * kHour - 1).valid);
    EXPECT_FALSE(store.authenticate(idle.id, "", 10 * kHour + 4 * kHour).valid);
    // Clock going backwards (shouldn't happen with uptime) fails closed.
    NewSession back;
    ASSERT_TRUE(store.create(20 * kHour, &back));
    EXPECT_FALSE(store.authenticate(back.id, "", 19 * kHour).valid);
}

TEST(AuthSession, CapacityEvictsLeastRecentlyUsed) {
    CountingRandom random;
    SessionStore store(random);
    std::vector<NewSession> sessions(kMaxSessions);
    for (size_t i = 0; i < kMaxSessions; i++) {
        ASSERT_TRUE(store.create(i, &sessions[i]));
    }
    EXPECT_EQ(store.activeCount(100), kMaxSessions);
    EXPECT_TRUE(store.authenticate(sessions[0].id, "", 100).valid);  // 0 is now the most recent
    NewSession extra;
    ASSERT_TRUE(store.create(200, &extra));
    EXPECT_EQ(store.activeCount(200), kMaxSessions);
    EXPECT_TRUE(store.authenticate(sessions[0].id, "", 201).valid);
    EXPECT_FALSE(store.authenticate(sessions[1].id, "", 201).valid) << "LRU slot should have been evicted";
    EXPECT_TRUE(store.authenticate(extra.id, "", 201).valid);
}

TEST(AuthSession, RebootLosesEverythingAndGarbageIsRejected) {
    CountingRandom random;
    NewSession s;
    {
        SessionStore store(random);
        ASSERT_TRUE(store.create(0, &s));
    }
    SessionStore afterReboot(random);  // RAM only: nothing survives
    EXPECT_FALSE(afterReboot.authenticate(s.id, "", 1).valid);
    EXPECT_FALSE(afterReboot.authenticate("", "", 1).valid);
    EXPECT_FALSE(afterReboot.authenticate(std::string(kSessionSecretLength, 'A'), "", 1).valid)
        << "an all-zero id must not match an empty slot";
    EXPECT_FALSE(afterReboot.authenticate("../../etc/passwd", "", 1).valid);
}

TEST(AuthSession, RemoveAllExcept) {
    CountingRandom random;
    SessionStore store(random);
    NewSession keep, other;
    ASSERT_TRUE(store.create(0, &keep));
    ASSERT_TRUE(store.create(0, &other));
    store.removeAllExcept(keep.id);
    EXPECT_TRUE(store.authenticate(keep.id, "", 1).valid);
    EXPECT_FALSE(store.authenticate(other.id, "", 1).valid);
    store.clear();
    EXPECT_EQ(store.activeCount(1), 0u);
}

TEST(AuthSession, FailsClosedWithoutEntropy) {
    CountingRandom random;
    SessionStore store(random);
    random.failing = true;
    NewSession s;
    EXPECT_FALSE(store.create(0, &s));
    EXPECT_EQ(store.activeCount(0), 0u);
}

// ---------------------------------------------------------------- rate limit

TEST(AuthRateLimit, FreeAttemptsThenDoublingLockout) {
    LoginRateLimiter limiter;  // 5 free, 1 s base, 15 min cap
    const uint64_t client = 0xC0A80138;
    uint64_t now = 1000;
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(limiter.retryAfterMs(client, now), 0u) << i;
        limiter.recordFailure(client, now);
    }
    EXPECT_EQ(limiter.retryAfterMs(client, now), 0u);
    limiter.recordFailure(client, now);  // 6th
    EXPECT_EQ(limiter.retryAfterMs(client, now), 1000u);
    now += 1000;
    EXPECT_EQ(limiter.retryAfterMs(client, now), 0u);
    limiter.recordFailure(client, now);  // 7th
    EXPECT_EQ(limiter.retryAfterMs(client, now), 2000u);
    now += 2000;
    limiter.recordFailure(client, now);  // 8th
    EXPECT_EQ(limiter.retryAfterMs(client, now), 4000u);

    // Other clients are unaffected.
    EXPECT_EQ(limiter.retryAfterMs(client + 1, now), 0u);

    // Success clears the record.
    limiter.recordSuccess(client);
    EXPECT_EQ(limiter.retryAfterMs(client, now), 0u);
}

TEST(AuthRateLimit, LockoutIsCapped) {
    RateLimitConfig config;
    config.globalMaxFailures = 1000000;
    LoginRateLimiter limiter(config);
    uint64_t now = 0;
    for (int i = 0; i < 60; i++) {
        now += limiter.retryAfterMs(7, now);
        limiter.recordFailure(7, now);
    }
    EXPECT_EQ(limiter.retryAfterMs(7, now), config.maxLockoutMs);
}

TEST(AuthRateLimit, GlobalLimitStopsAddressRotation) {
    LoginRateLimiter limiter;  // 30 failures per minute globally
    for (uint64_t client = 0; client < 30; client++) {
        EXPECT_EQ(limiter.retryAfterMs(client, 5000), 0u);
        limiter.recordFailure(client, 5000);
    }
    EXPECT_EQ(limiter.retryAfterMs(999, 5000), 60000u);
    EXPECT_EQ(limiter.retryAfterMs(999, 64999), 1u);
    EXPECT_EQ(limiter.retryAfterMs(999, 65000), 0u);
}

TEST(AuthRateLimit, RecordsAreForgottenAndTableRecycles) {
    LoginRateLimiter limiter;
    for (int i = 0; i < 6; i++) limiter.recordFailure(1, 0);
    EXPECT_GT(limiter.retryAfterMs(1, 0), 0u);
    // After forgetAfterMs with no failures the counter starts over.
    const uint64_t later = RateLimitConfig{}.forgetAfterMs + 1;
    EXPECT_EQ(limiter.retryAfterMs(1, later), 0u);
    limiter.recordFailure(1, later);
    EXPECT_EQ(limiter.retryAfterMs(1, later), 0u) << "fresh record, first failure is free";

    // More clients than slots: still bounded, still works.
    for (uint64_t c = 100; c < 100 + 3 * LoginRateLimiter::kSlots; c++) {
        limiter.recordFailure(c, later + 120000 * c);
    }
}
