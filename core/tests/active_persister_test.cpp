// Remembering the active activity across reboots: delayed, coalesced
// writes off the hot path, and a silent restore at boot.
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "fixtures.h"
#include "hub.h"
#include "hubbo/config/json.h"
#include "hubbo/engine/active_persister.h"
#include "hubbo/storage/state_store.h"
#include "auth_test_util.h"

using namespace hubbo;
using namespace hubbo::sim;
using remote::Button;

TEST(StateStore, KeysAndBackends) {
    EXPECT_TRUE(storage::isValidKey("active_activity"));
    EXPECT_FALSE(storage::isValidKey("active_activity_x"));  // 16 > NVS's 15
    EXPECT_FALSE(storage::isValidKey("Active"));
    EXPECT_FALSE(storage::isValidKey(""));

    storage::MemoryStorage disk;
    storage::SlotRecordStore records(disk, 1024);
    storage::RecordStateStore state(records);
    std::string value;
    EXPECT_EQ(state.get("active_activity", &value), ErrorCode::NotFound);
    ASSERT_TRUE(state.set("active_activity", "act_watch_apple_tv"));
    ASSERT_EQ(state.get("active_activity", &value), ErrorCode::Ok);
    EXPECT_EQ(value, "act_watch_apple_tv");
    EXPECT_EQ(disk.files().count("active_activity.a"), 1u);
    EXPECT_FALSE(state.set("active_activity", std::string(65, 'x')));
}

TEST(ActivePersister, WritesOnceStableAndOnlyWhenChanged) {
    storage::MemoryStateStore store;
    ActivePersister p(store, 5000);
    std::string saved = "x";
    EXPECT_EQ(p.savedActivity(&saved), ErrorCode::NotFound);
    EXPECT_EQ(saved, "");
    p.tick("", 0, true);
    p.tick("", 10000, true);
    EXPECT_EQ(store.writes(), 0);  // nothing changed

    p.tick("act_a", 100, true);
    p.tick("act_a", 5099, true);
    EXPECT_EQ(store.writes(), 0);
    p.tick("act_a", 5100, true);
    EXPECT_EQ(store.writes(), 1);
    std::string v;
    store.get(ActivePersister::kKey, &v);
    EXPECT_EQ(v, "act_a");

    // Two switches inside the delay: one write, of the last one.
    p.tick("act_b", 6000, true);
    p.tick("act_c", 7000, true);
    p.tick("act_c", 12000, true);
    EXPECT_EQ(store.writes(), 2);
    store.get(ActivePersister::kKey, &v);
    EXPECT_EQ(v, "act_c");

    // Away and back inside the delay: no write.
    p.tick("act_d", 13000, true);
    p.tick("act_c", 14000, true);
    p.tick("act_c", 30000, true);
    EXPECT_EQ(store.writes(), 2);

    // Off saves "".
    p.tick("", 31000, true);
    p.tick("", 36000, true);
    EXPECT_EQ(store.writes(), 3);
    store.get(ActivePersister::kKey, &v);
    EXPECT_EQ(v, "");
}

TEST(ActivePersister, RetriesAFailedWrite) {
    storage::MemoryStateStore store;
    ActivePersister p(store, 5000);
    std::string saved;
    p.savedActivity(&saved);
    store.failWrites = true;
    p.tick("act_a", 0, true);
    p.tick("act_a", 5000, true);
    EXPECT_TRUE(p.pending());
    store.failWrites = false;
    p.tick("act_a", 9999, true);
    EXPECT_EQ(store.writes(), 0);
    p.tick("act_a", 10000, true);
    EXPECT_EQ(store.writes(), 1);
    EXPECT_FALSE(p.pending());
}

namespace {

class RebootTest : public ::testing::Test {
protected:
    RebootTest() { start(); }
    void start() {
        hub = std::make_unique<SimHub>(&disk, &state);
        hub->boot(fixtureConfig());
    }
    void reboot() {
        hub.reset();
        start();
    }

    storage::MemoryStorage disk;
    storage::MemoryStateStore state;
    std::unique_ptr<SimHub> hub;
};

}  // namespace

TEST_F(RebootTest, RestoresSilently) {
    hub->activate("act_watch_apple_tv");
    hub->press(Button::Up);  // a key held across the reboot
    hub->advance(6000);
    reboot();
    EXPECT_STREQ(hub->engine().activeActivityId(), "act_watch_apple_tv");
    EXPECT_EQ(hub->engine().activeActivityIdShared(), "act_watch_apple_tv");
    EXPECT_TRUE(hub->commands().empty());       // no startup
    EXPECT_TRUE(hub->hid().reports().empty());  // no releaseAll
    EXPECT_EQ(hub->engine().generation(), 0u);
    EXPECT_EQ(hub->engine().executor().stats().enqueued, 0u);
}

TEST_F(RebootTest, TheSwitchItselfNeverWrites) {
    const int writes = state.writes();
    const int fileWrites = disk.mutations();
    hub->activate("act_watch_apple_tv");
    hub->tap(Button::Music);  // switch by button
    EXPECT_EQ(state.writes(), writes);
    EXPECT_EQ(disk.mutations(), fileWrites);
    hub->advance(6000);
    EXPECT_EQ(state.writes(), writes + 1);  // one delayed write for both switches
    EXPECT_EQ(disk.mutations(), fileWrites);  // config untouched
}

TEST_F(RebootTest, DeletedActivityBootsWithNothingActive) {
    hub->activate("act_play_xbox");
    hub->advance(6000);
    // Delete it behind the engine's back, as a restore with another config would.
    ConfigSpec cfg = fixtureConfig();
    cfg.activities.pop_back();
    cfg.globalBindings.erase(cfg.globalBindings.begin() + 3);  // music:tap referenced it
    storage::MemoryStorage other;
    {
        SimHub editor(&other);
        ASSERT_TRUE(editor.load(cfg).ok());
    }
    // Swap in that config record; keep the saved active activity.
    disk.files().erase("config.a");
    disk.files().erase("config.b");
    for (const char *slot : {"config.a", "config.b"}) {
        if (other.files().count(slot) != 0) disk.files()[slot] = other.files()[slot];
    }
    reboot();
    EXPECT_FALSE(hub->engine().hasActiveActivity());
    EXPECT_TRUE(hub->commands().empty());
}

TEST_F(RebootTest, OffIsRememberedAsNone) {
    hub->activate("act_play_xbox");
    hub->advance(6000);
    hub->off();
    hub->advance(6000);
    reboot();
    EXPECT_FALSE(hub->engine().hasActiveActivity());
}

TEST_F(RebootTest, RestoreUnknownLeavesNothingActive) {
    EXPECT_EQ(hub->engine().restoreActive("act_nope", hub->nowMs()), ErrorCode::UnknownActivity);
    EXPECT_FALSE(hub->engine().hasActiveActivity());
    EXPECT_EQ(hub->engine().restoreActive("act_play_xbox", hub->nowMs()), ErrorCode::Ok);
    EXPECT_TRUE(hub->commands().empty());
}

TEST_F(RebootTest, SharedActiveIdIsSafeFromAnotherThread) {
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        while (!stop) {
            const std::string id = hub->engine().activeActivityIdShared();
            EXPECT_TRUE(id.empty() || id == "act_watch_apple_tv" || id == "act_play_xbox") << id;
        }
    });
    for (int i = 0; i < 200; i++) {
        hub->activate(i % 2 ? "act_watch_apple_tv" : "act_play_xbox");
        if (i % 7 == 0) hub->off();
        hub->advance(1);
    }
    stop = true;
    reader.join();
}

TEST(ActivePersister, NothingIsWrittenWhileTheConfigIsNotOk) {
    storage::MemoryStateStore store;
    ActivePersister p(store, 5000);
    std::string saved;
    p.savedActivity(&saved);
    p.tick("act_a", 0, false);
    p.tick("act_a", 60000, false);
    EXPECT_EQ(store.writes(), 0);
    p.flush("act_a", false);
    EXPECT_EQ(store.writes(), 0);
    p.tick("act_a", 60100, true);  // Ok again: the stable change is written
    EXPECT_EQ(store.writes(), 1);
}

TEST(ActivePersister, AnUnreadableSavedValueIsUnknownNotNone) {
    storage::MemoryStateStore store;
    ASSERT_TRUE(store.set(ActivePersister::kKey, "act_tv"));
    store.failReads = true;
    ActivePersister p(store, 5000);
    std::string saved = "x";
    EXPECT_EQ(p.savedActivity(&saved), ErrorCode::StorageFailed);
    EXPECT_EQ(saved, "");
    p.holdUntilChanged("");  // the boot couldn't restore: "" means unknown
    p.tick("", 0, true);
    p.tick("", 60000, true);
    p.flush("", true);
    EXPECT_EQ(store.writes(), 1);  // only the setup write
    store.failReads = false;
    std::string v;
    ASSERT_EQ(store.get(ActivePersister::kKey, &v), ErrorCode::Ok);
    EXPECT_EQ(v, "act_tv");
    // A real change ends the hold.
    p.tick("act_b", 61000, true);
    p.tick("act_b", 66000, true);
    ASSERT_EQ(store.get(ActivePersister::kKey, &v), ErrorCode::Ok);
    EXPECT_EQ(v, "act_b");
}

TEST_F(RebootTest, ABlockedBootKeepsTheSavedActivity) {
    hub->activate("act_watch_apple_tv");
    hub->advance(6000);
    std::string v;
    ASSERT_EQ(state.get(ActivePersister::kKey, &v), ErrorCode::Ok);
    ASSERT_EQ(v, "act_watch_apple_tv");
    const int writes = state.writes();

    // A config record from a newer firmware: the next boot is Blocked.
    std::string newer = config::serializeConfig(fixtureConfig());
    newer.replace(newer.find("\"schema_version\":2"), 18, "\"schema_version\":9");
    uint64_t generation = 0;
    ASSERT_EQ(hub->records().nextGeneration(config::ConfigService::kRecordName, &generation), ErrorCode::Ok);
    ASSERT_EQ(hub->records().save(config::ConfigService::kRecordName, generation, newer), ErrorCode::Ok);
    reboot();
    ASSERT_EQ(hub->configService().state(), config::ConfigState::Blocked);
    EXPECT_FALSE(hub->engine().hasActiveActivity());
    hub->advance(10000);
    hub->flush();
    EXPECT_EQ(state.writes(), writes);  // "" is unknown, not none
    ASSERT_EQ(state.get(ActivePersister::kKey, &v), ErrorCode::Ok);
    EXPECT_EQ(v, "act_watch_apple_tv");

    // A restore unblocks it; nothing changed the active activity, so the
    // saved one survives and comes back at the next boot.
    ASSERT_TRUE(hub->load(fixtureConfig()).ok());
    ASSERT_EQ(hub->configService().state(), config::ConfigState::Ok);
    hub->advance(10000);
    hub->flush();
    EXPECT_EQ(state.writes(), writes);
    ASSERT_EQ(state.get(ActivePersister::kKey, &v), ErrorCode::Ok);
    EXPECT_EQ(v, "act_watch_apple_tv");
    reboot();
    EXPECT_STREQ(hub->engine().activeActivityId(), "act_watch_apple_tv");
}

TEST_F(RebootTest, AnUnreadableSavedActivityIsNeverOverwrittenWithNone) {
    hub->activate("act_watch_apple_tv");
    hub->advance(6000);
    const int writes = state.writes();
    state.failReads = true;
    reboot();
    EXPECT_FALSE(hub->engine().hasActiveActivity());
    hub->advance(10000);
    hub->flush();
    EXPECT_EQ(state.writes(), writes);
    state.failReads = false;
    reboot();
    EXPECT_STREQ(hub->engine().activeActivityId(), "act_watch_apple_tv");
}

// ---- one boot path: bootHub() (design part 6) ----

TEST_F(RebootTest, BootReportsWhatItRestored) {
    EXPECT_EQ(hub->lastBoot().active, ActiveRestore::NoneSaved);
    hub->activate("act_watch_apple_tv");
    hub->advance(6000);
    reboot();
    EXPECT_EQ(hub->lastBoot().config.state, config::ConfigState::Ok);
    EXPECT_EQ(hub->lastBoot().active, ActiveRestore::Restored);
    EXPECT_EQ(hub->lastBoot().activityId, "act_watch_apple_tv");
    EXPECT_EQ(hub->lastBoot().credentials, auth::CredentialState::Unavailable);  // none given

    state.failReads = true;
    reboot();
    EXPECT_EQ(hub->lastBoot().active, ActiveRestore::Unknown);
    EXPECT_EQ(hub->lastBoot().reason, ErrorCode::StorageFailed);
}

TEST(BootHub, AFreshConfigNeverRestoresTheSavedActivity) {
    // A saved activity but no config record (e.g. the config was lost):
    // restore only into an Ok config; the saved value is kept (unknown).
    storage::MemoryStorage disk;
    storage::MemoryStateStore state;
    ASSERT_TRUE(state.set(ActivePersister::kKey, "act_watch_apple_tv"));
    SimHub hub(&disk, &state);
    hub.boot(fixtureConfig());
    EXPECT_EQ(hub.lastBoot().config.state, config::ConfigState::Fresh);
    EXPECT_EQ(hub.lastBoot().active, ActiveRestore::Unknown);
    EXPECT_EQ(hub.lastBoot().reason, ErrorCode::ConfigBlocked);
    EXPECT_FALSE(hub.engine().hasActiveActivity());
    hub.advance(10000);
    std::string v;
    ASSERT_EQ(state.get(ActivePersister::kKey, &v), ErrorCode::Ok);
    EXPECT_EQ(v, "act_watch_apple_tv");
}

TEST(BootHub, ACorruptSavedActivityIsUnknown) {
    storage::MemoryStorage disk;
    disk.files()["active_activity.a"] = "garbage";
    SimHub hub(&disk);
    hub.boot(fixtureConfig());
    EXPECT_EQ(hub.lastBoot().active, ActiveRestore::Unknown);
    EXPECT_EQ(hub.lastBoot().reason, ErrorCode::Corrupt);
    const std::string before = disk.files()["active_activity.a"];
    hub.advance(10000);
    hub.flush();
    EXPECT_EQ(disk.files()["active_activity.a"], before);  // never overwritten with "none"
    EXPECT_EQ(disk.files().count("active_activity.b"), 0u);
}

TEST(BootHub, LoadsTheCredentialsBetweenConfigAndActivity) {
    storage::MemoryStorage disk;
    auth::MemorySecretStore secrets;
    auth::SoftwareHash hash;
    auth::test::CountingRandom random;
    auth::Credentials credentials(secrets, hash, random);
    SimHub hub(&disk);
    hub.boot(fixtureConfig(), &credentials);
    EXPECT_EQ(hub.lastBoot().credentials, auth::CredentialState::Fresh);
    EXPECT_EQ(credentials.state(), auth::CredentialState::Fresh);
}
