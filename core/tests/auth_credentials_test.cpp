#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "auth_test_util.h"
#include "hubbo/api/authenticator.h"
#include "hubbo/auth/credentials.h"
#include "hubbo/auth/secret_store.h"
#include "hubbo/storage/record_store.h"
#include "hubbo/storage/storage.h"

using namespace hubbo;
using namespace hubbo::auth;
using hubbo::auth::test::CountingRandom;
using hubbo::auth::test::FakeClock;
using hubbo::auth::test::Headers;

namespace {

constexpr uint32_t kFastIterations = kMinPasswordIterations;

struct Fixture {
    Fixture() { creds.load(); }
    // A fixture with the admin already set up (tokens need one).
    void withAdmin() { ASSERT_EQ(creds.setupAdmin("admin", "correct horse battery"), CredentialResult::Ok); }
    MemorySecretStore store;
    SoftwareHash hash;
    CountingRandom random;
    Credentials creds{store, hash, random, kFastIterations};
};

}  // namespace

TEST(AuthSecretStore, KeysFollowNvsRules) {
    MemorySecretStore s;
    EXPECT_TRUE(s.write("admin", "x"));
    EXPECT_FALSE(s.write("Admin", "x"));
    EXPECT_FALSE(s.write("this_key_is_too_long", "x"));
    EXPECT_FALSE(s.write("", "x"));
    EXPECT_FALSE(s.write("big", std::string(kMaxSecretValueLength + 1, 'a')));
    EXPECT_TRUE(s.write("tok_a", "1"));
    EXPECT_TRUE(s.write("tok_b", "2"));
    std::vector<std::string> keys;
    EXPECT_EQ(s.keys("tok_", &keys), SecretRead::Ok);
    EXPECT_EQ(keys.size(), 2u);
    EXPECT_TRUE(s.erase("tok_a"));
    EXPECT_TRUE(s.erase("tok_a")) << "erasing a missing key succeeds";
    std::string v;
    EXPECT_EQ(s.read("tok_a", &v), SecretRead::NotFound);
    // Unreadable is not absent.
    s.healthState = storage::Health::Unavailable;
    EXPECT_EQ(s.read("tok_b", &v), SecretRead::Unavailable);
    EXPECT_EQ(s.keys("tok_", &keys), SecretRead::Unavailable);
    EXPECT_FALSE(s.write("tok_c", "3"));
}

TEST(AuthCredentials, SetupLoginAndPersistence) {
    Fixture f;
    EXPECT_EQ(f.creds.state(), CredentialState::Fresh);
    EXPECT_NE(f.creds.state(), CredentialState::Ok);
    EXPECT_FALSE(f.creds.verifyLogin("admin", "anything at all"));
    EXPECT_EQ(f.creds.setupAdmin("admin", "short"), CredentialResult::WeakPassword);
    EXPECT_EQ(f.creds.setupAdmin("bad name", "correct horse battery"), CredentialResult::InvalidUsername);
    ASSERT_EQ(f.creds.setupAdmin("admin", "correct horse battery"), CredentialResult::Ok);
    EXPECT_EQ(f.creds.setupAdmin("admin2", "correct horse battery"), CredentialResult::AlreadyExists);

    EXPECT_TRUE(f.creds.verifyLogin("admin", "correct horse battery"));
    EXPECT_FALSE(f.creds.verifyLogin("admin", "wrong horse battery"));
    EXPECT_FALSE(f.creds.verifyLogin("root", "correct horse battery"));

    // Only a hash is stored.
    std::string stored;
    ASSERT_EQ(f.store.read("admin", &stored), SecretRead::Ok);
    EXPECT_EQ(stored.find("correct horse"), std::string::npos);
    EXPECT_EQ(stored.rfind("admin|pbkdf2-sha256$", 0), 0u) << "one key: username and hash together";

    // "Reboot": a new Credentials over the same store.
    Credentials again(f.store, f.hash, f.random, kFastIterations);
    again.load();
    EXPECT_EQ(again.state(), CredentialState::Ok);
    EXPECT_TRUE(again.verifyLogin("admin", "correct horse battery"));
}

TEST(AuthCredentials, ChangePassword) {
    Fixture f;
    ASSERT_EQ(f.creds.setupAdmin("admin", "correct horse battery"), CredentialResult::Ok);
    EXPECT_EQ(f.creds.changePassword("nope nope nope", "a whole new password"), CredentialResult::WrongPassword);
    EXPECT_EQ(f.creds.changePassword("correct horse battery", "short"), CredentialResult::WeakPassword);
    ASSERT_EQ(f.creds.changePassword("correct horse battery", "a whole new password"), CredentialResult::Ok);
    EXPECT_FALSE(f.creds.verifyLogin("admin", "correct horse battery"));
    EXPECT_TRUE(f.creds.verifyLogin("admin", "a whole new password"));
}

TEST(AuthCredentials, FailedWritesChangeNothing) {
    Fixture f;
    f.store.failWrites = true;
    EXPECT_EQ(f.creds.setupAdmin("admin", "correct horse battery"), CredentialResult::StorageFailed);
    EXPECT_NE(f.creds.state(), CredentialState::Ok);
    f.store.failWrites = false;
    ASSERT_EQ(f.creds.setupAdmin("admin", "correct horse battery"), CredentialResult::Ok);
    f.store.failWrites = true;
    IssuedToken t;
    EXPECT_EQ(f.creds.createToken("agent", 100, &t), CredentialResult::StorageFailed);
    EXPECT_TRUE(f.creds.tokens().empty());
    EXPECT_EQ(f.creds.changePassword("correct horse battery", "a whole new password"),
              CredentialResult::StorageFailed);
    EXPECT_TRUE(f.creds.verifyLogin("admin", "correct horse battery"));
}

TEST(AuthCredentials, TokensLifecycle) {
    Fixture f;
    f.withAdmin();
    IssuedToken a;
    ASSERT_EQ(f.creds.createToken("Claude agent", 1762600000, &a), CredentialResult::Ok);
    EXPECT_EQ(f.creds.createToken("", 0, &a), CredentialResult::InvalidName);
    EXPECT_EQ(f.creds.createToken("bad\nname", 0, &a), CredentialResult::InvalidName);
    ASSERT_EQ(f.creds.tokens().size(), 1u);
    const std::string id = f.creds.tokens()[0].id;
    EXPECT_EQ(f.creds.tokens()[0].createdAt, 1762600000);
    EXPECT_EQ(f.creds.tokens()[0].lastUsedAt, -1);

    // The plaintext is not in the store.
    std::string record;
    ASSERT_EQ(f.store.read("tok_" + id, &record), SecretRead::Ok);
    EXPECT_EQ(record.find(a.plaintext.substr(13)), std::string::npos);

    const StoredToken *used = f.creds.authenticateToken(a.plaintext, 1762600500);
    ASSERT_NE(used, nullptr);
    EXPECT_EQ(used->lastUsedAt, 1762600500);
    EXPECT_EQ(f.creds.authenticateToken("hbt_" + id + "_" + std::string(43, 'A'), 0), nullptr);

    // Survives a reload (last-used doesn't: RAM only).
    Credentials again(f.store, f.hash, f.random, kFastIterations);
    again.load();
    ASSERT_NE(again.authenticateToken(a.plaintext, 1), nullptr);
    EXPECT_EQ(again.tokens()[0].name, "Claude agent");

    ASSERT_EQ(again.revokeToken(id), CredentialResult::Ok);
    EXPECT_EQ(again.revokeToken(id), CredentialResult::NotFound);
    EXPECT_EQ(again.authenticateToken(a.plaintext, 1), nullptr);
    Credentials third(f.store, f.hash, f.random, kFastIterations);
    third.load();
    EXPECT_TRUE(third.tokens().empty());
}

TEST(AuthCredentials, TokenLimitAndCorruptRecords) {
    Fixture f;
    f.withAdmin();
    for (size_t i = 0; i < kMaxApiTokens; i++) {
        IssuedToken t;
        ASSERT_EQ(f.creds.createToken("t" + std::to_string(i), 0, &t), CredentialResult::Ok);
    }
    IssuedToken extra;
    EXPECT_EQ(f.creds.createToken("one too many", 0, &extra), CredentialResult::LimitReached);

    // Malformed token records are skipped (fail closed).
    ASSERT_TRUE(f.store.write("tok_garbage1", "not a record"));
    ASSERT_TRUE(f.store.write("tok_garbage2", "AAAA|1|x"));
    Credentials c(f.store, f.hash, f.random, kFastIterations);
    c.load();
    EXPECT_EQ(c.state(), CredentialState::Ok);
    EXPECT_EQ(c.tokens().size(), kMaxApiTokens);
}

// ---------------------------------------------------------------- states
// "Can't read" is never "doesn't exist": only Fresh may create the admin.

TEST(AuthCredentialStates, UnreadableStoreIsUnavailableNotFresh) {
    Fixture f;
    f.withAdmin();
    f.store.healthState = storage::Health::Unavailable;
    Credentials boot(f.store, f.hash, f.random, kFastIterations);
    boot.load();
    EXPECT_EQ(boot.state(), CredentialState::Unavailable);
    EXPECT_NE(boot.state(), CredentialState::Ok);
    // The takeover: writes work again, but credentials are read once at
    // boot. Setup still refuses, never overwriting the real admin.
    f.store.healthState = storage::Health::Available;
    EXPECT_EQ(boot.setupAdmin("mallory", "a password of mine"), CredentialResult::Unavailable);
    EXPECT_EQ(boot.state(), CredentialState::Unavailable);
    // A reboot recovers.
    Credentials reboot(f.store, f.hash, f.random, kFastIterations);
    reboot.load();
    EXPECT_EQ(reboot.state(), CredentialState::Ok);
    EXPECT_TRUE(reboot.verifyLogin("admin", "correct horse battery"));
    EXPECT_FALSE(reboot.verifyLogin("mallory", "a password of mine"));
}

TEST(AuthCredentialStates, NothingIsWrittenWhileUnavailable) {
    Fixture f;
    f.withAdmin();
    f.store.healthState = storage::Health::Unavailable;
    Credentials boot(f.store, f.hash, f.random, kFastIterations);
    boot.load();
    EXPECT_EQ(boot.setupAdmin("mallory", "a password of mine"), CredentialResult::Unavailable);
    IssuedToken t;
    EXPECT_EQ(boot.createToken("x", 0, &t), CredentialResult::Unavailable);
    EXPECT_EQ(boot.changePassword("correct horse battery", "another password"), CredentialResult::Unavailable);
    EXPECT_FALSE(boot.verifyLogin("admin", "correct horse battery"));
}

TEST(AuthCredentialStates, DamagedRecordsAreUnavailableAndBlockSetup) {
    {
        Fixture f;
        ASSERT_TRUE(f.store.write("admin", "no separator"));
        f.creds.load();
        EXPECT_EQ(f.creds.state(), CredentialState::Unavailable);
        EXPECT_EQ(f.creds.setupAdmin("mallory", "a password of mine"), CredentialResult::Unavailable);
    }
    {
        // Tokens without an admin: the admin record was lost, not never made.
        Fixture f;
        ASSERT_TRUE(f.store.write("tok_abcdefgh", "AAAA|1|x"));
        f.creds.load();
        EXPECT_EQ(f.creds.state(), CredentialState::Unavailable);
        EXPECT_EQ(f.creds.setupAdmin("mallory", "a password of mine"), CredentialResult::Unavailable);
    }
}

TEST(AuthCredentialStates, SetupTakesTheUsersOwnUsername) {
    Fixture f;
    EXPECT_EQ(f.creds.setupAdmin("bad name", "correct horse battery"), CredentialResult::InvalidUsername);
    EXPECT_EQ(f.creds.setupAdmin("", "correct horse battery"), CredentialResult::InvalidUsername);
    ASSERT_EQ(f.creds.setupAdmin("jon.m", "correct horse battery"), CredentialResult::Ok);
    EXPECT_EQ(f.creds.username(), "jon.m");
    EXPECT_FALSE(f.creds.verifyLogin("admin", "correct horse battery")) << "no built-in admin name";
    EXPECT_TRUE(f.creds.verifyLogin("jon.m", "correct horse battery"));
}

TEST(AuthCredentialStates, FreshOnlyWhenPositivelyEmpty) {
    Fixture f;
    EXPECT_EQ(f.creds.state(), CredentialState::Fresh);
    EXPECT_EQ(f.creds.setupAdmin("admin", "correct horse battery"), CredentialResult::Ok);
    EXPECT_EQ(f.creds.state(), CredentialState::Ok);
}

// ---------------------------------------------------------------- authenticator

namespace {

}  // namespace

TEST(ApiAuthenticator, BearerAndCookie) {
    Fixture f;
    ASSERT_EQ(f.creds.setupAdmin("admin", "correct horse battery"), CredentialResult::Ok);
    IssuedToken token;
    ASSERT_EQ(f.creds.createToken("agent", 0, &token), CredentialResult::Ok);
    SessionStore sessions(f.random);
    FakeClock clock;
    std::string session;
    ASSERT_TRUE(sessions.create(clock.mono, &session));
    api::Authenticator authn(f.creds, sessions, clock);

    auto run = [&](std::map<std::string, std::string> h) {
        Headers headers;
        headers.items = std::move(h);
        http::Request r;
        r.headers = &headers;
        return authn.authenticate(r);
    };

    RequestContext c = run({});
    EXPECT_EQ(c.principal.role, Role::Anonymous);

    c = run({{"authorization", "Bearer " + token.plaintext}});
    EXPECT_EQ(c.principal.role, Role::Token);
    EXPECT_EQ(c.principal.credential, Credential::BearerToken);

    c = run({{"cookie", "theme=dark; hubbo_session=" + session}});
    EXPECT_EQ(c.principal.role, Role::Admin);
    EXPECT_EQ(c.sessionExpiresInMs, kSessionLifetimeMs);

    // A bad bearer never falls back to the cookie.
    c = run({{"authorization", "Bearer hbt_nope"}, {"cookie", "hubbo_session=" + session}});
    EXPECT_EQ(c.principal.role, Role::Anonymous);
    // Expired session.
    clock.mono += kSessionLifetimeMs;
    c = run({{"cookie", "hubbo_session=" + session}});
    EXPECT_EQ(c.principal.role, Role::Anonymous);
}

TEST(ApiAuthenticator, CookieParsing) {
    EXPECT_EQ(api::cookieValue("a=1; hubbo_session=xyz; b=2", "hubbo_session"), "xyz");
    EXPECT_EQ(api::cookieValue("hubbo_session=xyz", "hubbo_session"), "xyz");
    EXPECT_EQ(api::cookieValue("xhubbo_session=xyz", "hubbo_session"), "");
    EXPECT_EQ(api::cookieValue("", "hubbo_session"), "");
}

// ---------------------------------------------------------------- persisted store

TEST(AuthStorageSecretStore, CredentialsSurviveRestart) {
    storage::MemoryStorage disk;
    storage::SlotRecordStore records(disk, 16 * 1024);
    SoftwareHash hash;
    CountingRandom random;
    std::string plaintext;
    std::string revokedPlaintext;
    {
        StorageSecretStore store(records);
        Credentials creds(store, hash, random, kFastIterations);
        creds.load();
        ASSERT_EQ(creds.setupAdmin("admin", "correct horse battery"), CredentialResult::Ok);
        IssuedToken keep, revoke;
        ASSERT_EQ(creds.createToken("agent", 100, &keep), CredentialResult::Ok);
        ASSERT_EQ(creds.createToken("old", 100, &revoke), CredentialResult::Ok);
        ASSERT_EQ(creds.revokeToken(revoke.id), CredentialResult::Ok);
        plaintext = keep.plaintext;
        revokedPlaintext = revoke.plaintext;
    }
    StorageSecretStore store(records);  // "reboot"
    Credentials creds(store, hash, random, kFastIterations);
    creds.load();
    EXPECT_EQ(creds.state(), CredentialState::Ok);
    EXPECT_TRUE(creds.verifyLogin("admin", "correct horse battery"));
    ASSERT_EQ(creds.tokens().size(), 1u);
    EXPECT_EQ(creds.tokens()[0].name, "agent");
    EXPECT_EQ(creds.tokens()[0].createdAt, 100);
    EXPECT_NE(creds.authenticateToken(plaintext, 1), nullptr);
    EXPECT_EQ(creds.authenticateToken(revokedPlaintext, 1), nullptr);
    for (const auto &file : disk.files()) {
        EXPECT_EQ(file.second.find("correct horse"), std::string::npos) << file.first << ": only hashes are stored";
    }
}

TEST(AuthStorageSecretStore, UnreadableOrCorruptRecordIsNeverOverwritten) {
    storage::MemoryStorage disk;
    storage::SlotRecordStore records(disk, 16 * 1024);
    {
        StorageSecretStore store(records);
        ASSERT_TRUE(store.write("admin", "admin|pbkdf2-sha256$x"));
    }
    // A record that isn't a JSON object: Corrupt, and writes are refused.
    ASSERT_EQ(records.save(StorageSecretStore::kRecordName, 99, "[1,2"), ErrorCode::Ok);
    StorageSecretStore corrupt(records);
    EXPECT_EQ(corrupt.loadState(), SecretRead::Corrupt);
    std::string v;
    EXPECT_EQ(corrupt.read("admin", &v), SecretRead::Corrupt);
    EXPECT_FALSE(corrupt.write("admin", "mallory|x"));
    storage::Record newest;
    ASSERT_EQ(records.load(StorageSecretStore::kRecordName, &newest), ErrorCode::Ok);
    EXPECT_EQ(newest.payload, "[1,2") << "nothing overwrote the record";
}

TEST(AuthStorageSecretStore, PowerLossKeepsOldOrNewNeverGarbage) {
    for (int cut = 0; cut < 8; cut++) {
        storage::MemoryStorage disk;
        storage::SlotRecordStore records(disk, 16 * 1024);
        {
            StorageSecretStore store(records);
            ASSERT_TRUE(store.write("admin", "admin"));
            disk.crashAfter(cut, 3);
            store.write("admin", "root");  // may fail mid-way
        }
        disk.reboot();
        StorageSecretStore store(records);
        std::string value;
        ASSERT_EQ(store.read("admin", &value), SecretRead::Ok) << "cut " << cut;
        EXPECT_TRUE(value == "admin" || value == "root") << "cut " << cut << ": " << value;
    }
}

TEST(AuthCredentialStates, ReadOnceAtBoot) {
    Fixture f;
    f.withAdmin();
    f.store.healthState = storage::Health::Unavailable;  // the medium goes away after boot
    EXPECT_EQ(f.creds.state(), CredentialState::Ok);
    EXPECT_TRUE(f.creds.verifyLogin("admin", "correct horse battery")) << "the hash is cached";
    IssuedToken t;
    EXPECT_EQ(f.creds.createToken("agent", 0, &t), CredentialResult::StorageFailed) << "but writes fail";
    EXPECT_TRUE(f.creds.tokens().empty());
    EXPECT_EQ(f.creds.setupAdmin("mallory", "a password of mine"), CredentialResult::AlreadyExists);
}

TEST(AuthStorageSecretStore, GenerationsComeFromTheStoreAndReadOnlyRefusesWrites) {
    storage::MemoryStorage disk;
    storage::SlotRecordStore records(disk, 16 * 1024);
    StorageSecretStore a(records);
    ASSERT_TRUE(a.write("admin", "one"));
    // Another writer bumps the record behind this store's back: the next save
    // must still go above it (no cached generation).
    uint64_t next = 0;
    ASSERT_EQ(records.nextGeneration(StorageSecretStore::kRecordName, &next), ErrorCode::Ok);
    ASSERT_EQ(records.save(StorageSecretStore::kRecordName, next + 5, R"({"admin":"two"})"), ErrorCode::Ok);
    ASSERT_TRUE(a.write("tok_x", "y"));
    storage::Record newest;
    ASSERT_EQ(records.load(StorageSecretStore::kRecordName, &newest), ErrorCode::Ok);
    EXPECT_GT(newest.generation, next + 5);
}

namespace {

// A record store whose health can be forced (e.g. ReadOnly after a failed
// slot invalidation); everything else goes to the real store.
struct ForcedHealthRecords : storage::RecordStore {
    explicit ForcedHealthRecords(storage::RecordStore &r) : real(r) {}
    storage::Health health() const override { return forced; }
    ErrorCode load(std::string_view name, storage::Record *out) override { return real.load(name, out); }
    ErrorCode nextGeneration(std::string_view name, uint64_t *out) override { return real.nextGeneration(name, out); }
    ErrorCode save(std::string_view name, uint64_t generation, std::string_view payload) override {
        return real.save(name, generation, payload);
    }
    ErrorCode remove(std::string_view name) override { return real.remove(name); }
    storage::RecordStore &real;
    storage::Health forced = storage::Health::Available;
};

}  // namespace

TEST(AuthStorageSecretStore, ReadOnlyStoreRefusesSecretWrites) {
    storage::MemoryStorage disk;
    storage::SlotRecordStore slots(disk, 16 * 1024);
    ForcedHealthRecords records(slots);
    StorageSecretStore store(records);
    ASSERT_TRUE(store.write("admin", "admin|pbkdf2-sha256$x"));
    storage::Record before;
    ASSERT_EQ(slots.load(StorageSecretStore::kRecordName, &before), ErrorCode::Ok);

    records.forced = storage::Health::ReadOnly;  // a slot couldn't be invalidated: no write is trusted
    EXPECT_FALSE(store.write("tok_abcdefgh", "x"));
    EXPECT_FALSE(store.erase("admin"));
    std::string value;
    EXPECT_EQ(store.read("admin", &value), SecretRead::Ok) << "reads still work on a read-only store";
    storage::Record after;
    ASSERT_EQ(slots.load(StorageSecretStore::kRecordName, &after), ErrorCode::Ok);
    EXPECT_EQ(after.generation, before.generation) << "nothing was saved";

    // Credentials on top report the failure instead of pretending.
    SoftwareHash hash;
    CountingRandom random;
    records.forced = storage::Health::Available;
    StorageSecretStore fresh(records);
    Credentials creds(fresh, hash, random, kFastIterations);
    creds.load();
    ASSERT_EQ(creds.state(), CredentialState::Ok);
    records.forced = storage::Health::ReadOnly;
    IssuedToken t;
    EXPECT_NE(creds.createToken("agent", 0, &t), CredentialResult::Ok);
    EXPECT_TRUE(creds.tokens().empty());
}

TEST(AuthStorageSecretStore, DamagedSlotsAreUnavailableNeverFresh) {
    // Absence needs proof (design part 6): secrets slots that exist but
    // aren't intact are Unavailable, so nobody gets to set up a new admin.
    for (const bool both : {true, false}) {
        storage::MemoryStorage disk;
        storage::SlotRecordStore records(disk, 16 * 1024);
        {
            StorageSecretStore store(records);
            SoftwareHash hash;
            CountingRandom random;
            Credentials creds(store, hash, random, kFastIterations);
            creds.load();
            ASSERT_EQ(creds.setupAdmin("admin", "correct horse battery"), CredentialResult::Ok);  // slot a
            IssuedToken t;
            ASSERT_EQ(creds.createToken("agent", 0, &t), CredentialResult::Ok);  // slot b
        }
        const char *damaged[] = {"secrets.b", "secrets.a"};
        for (size_t i = 0; i < (both ? 2u : 1u); i++) {
            std::string &slot = disk.files()[damaged[i]];
            ASSERT_FALSE(slot.empty()) << damaged[i];
            slot[slot.size() / 2] = static_cast<char>(slot[slot.size() / 2] ^ 0x5A);
        }
        const std::map<std::string, std::string> before = disk.files();
        StorageSecretStore store(records);
        SoftwareHash hash;
        CountingRandom random;
        Credentials creds(store, hash, random, kFastIterations);
        creds.load();
        if (!both) {
            // One torn slot next to an intact one: the older record loads.
            EXPECT_EQ(creds.state(), CredentialState::Ok);
            EXPECT_TRUE(creds.verifyLogin("admin", "correct horse battery"));
            continue;
        }
        EXPECT_EQ(store.loadState(), SecretRead::Corrupt);
        EXPECT_EQ(creds.state(), CredentialState::Unavailable);
        EXPECT_EQ(creds.setupAdmin("mallory", "a password of mine"), CredentialResult::Unavailable);
        EXPECT_FALSE(creds.verifyLogin("admin", "correct horse battery"));
        EXPECT_EQ(disk.files(), before) << "nothing overwrote the slots";
    }
}
