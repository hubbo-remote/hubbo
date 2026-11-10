#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "auth_test_util.h"
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

TEST(AuthSession, CreateAuthenticateLogout) {
    CountingRandom random;
    SessionStore store(random);
    std::string id;
    ASSERT_TRUE(store.create(1000, &id));
    EXPECT_EQ(id.size(), kSessionSecretLength);

    const SessionAuth ok = store.authenticate(id, 2000);
    EXPECT_TRUE(ok.valid);
    EXPECT_EQ(ok.expiresInMs, kSessionLifetimeMs - 1000);

    store.remove(id);
    EXPECT_FALSE(store.authenticate(id, 3000).valid);
}

TEST(AuthSession, OneAbsoluteLifetimeNoIdleTimer) {
    CountingRandom random;
    SessionStore store(random, 5 * kHour);
    std::string id;
    ASSERT_TRUE(store.create(0, &id));
    // Unused for hours, still valid...
    EXPECT_TRUE(store.authenticate(id, 5 * kHour - 1).valid);
    // ...until the lifetime ends, however often it's used.
    EXPECT_FALSE(store.authenticate(id, 5 * kHour).valid);
    EXPECT_EQ(kSessionLifetimeMs, 7 * 24 * kHour);
    // Clock going backwards (shouldn't happen with uptime) fails closed.
    std::string back;
    ASSERT_TRUE(store.create(20 * kHour, &back));
    EXPECT_FALSE(store.authenticate(back, 19 * kHour).valid);
}

TEST(AuthSession, CapacityEvictsTheOldest) {
    CountingRandom random;
    SessionStore store(random);
    std::vector<std::string> sessions(kMaxSessions);
    for (size_t i = 0; i < kMaxSessions; i++) {
        ASSERT_TRUE(store.create(i, &sessions[i]));
    }
    EXPECT_EQ(store.activeCount(100), kMaxSessions);
    std::string extra;
    ASSERT_TRUE(store.create(200, &extra));
    EXPECT_EQ(store.activeCount(200), kMaxSessions);
    EXPECT_FALSE(store.authenticate(sessions[0], 201).valid) << "the oldest should have been evicted";
    EXPECT_TRUE(store.authenticate(sessions[1], 201).valid);
    EXPECT_TRUE(store.authenticate(extra, 201).valid);
}

TEST(AuthSession, RebootLosesEverythingAndGarbageIsRejected) {
    CountingRandom random;
    std::string id;
    {
        SessionStore store(random);
        ASSERT_TRUE(store.create(0, &id));
    }
    SessionStore afterReboot(random);  // RAM only: nothing survives
    EXPECT_FALSE(afterReboot.authenticate(id, 1).valid);
    EXPECT_FALSE(afterReboot.authenticate("", 1).valid);
    EXPECT_FALSE(afterReboot.authenticate(std::string(kSessionSecretLength, 'A'), 1).valid)
        << "an all-zero id must not match an empty slot";
    EXPECT_FALSE(afterReboot.authenticate("../../etc/passwd", 1).valid);
}

TEST(AuthSession, RemoveAllExcept) {
    CountingRandom random;
    SessionStore store(random);
    std::string keep, other;
    ASSERT_TRUE(store.create(0, &keep));
    ASSERT_TRUE(store.create(0, &other));
    store.removeAllExcept(keep);
    EXPECT_TRUE(store.authenticate(keep, 1).valid);
    EXPECT_FALSE(store.authenticate(other, 1).valid);
    store.clear();
    EXPECT_EQ(store.activeCount(1), 0u);
}

TEST(AuthSession, FailsClosedWithoutEntropy) {
    CountingRandom random;
    SessionStore store(random);
    random.failing = true;
    std::string id;
    EXPECT_FALSE(store.create(0, &id));
    EXPECT_EQ(store.activeCount(0), 0u);
}
