// ConfigService: boot/load, create/replace/delete with optimistic
// revisions, referrers, rollback on failure, command compilation and the
// "no flash writes on button press or activity switch" rule.
#include <gtest/gtest.h>

#include <algorithm>

#include "fixtures.h"
#include "hub.h"
#include "hubbo/config/service.h"
#include "hubbo/storage/storage.h"

using namespace hubbo;
using namespace hubbo::config;
using namespace hubbo::sim;
using remote::Button;

namespace {

class ServiceTest : public ::testing::Test {
protected:
    ServiceTest() { boot(); }

    // A fresh service and store over the same storage: a simulated reboot.
    void boot() {
        engineReader = std::make_unique<Engine::Store::Reader>();
        store = std::make_unique<Engine::Store>();
        store->attach(*engineReader);
        records = std::make_unique<storage::SlotRecordStore>(mem, kMaxConfigFileBytes);
        service = std::make_unique<ConfigService>(*records, fakes.set, *store);
        service->setListener(listener);
        report = service->load(fixtureConfig());
    }

    const CompiledConfig *published() {
        guard.reset();  // a nested read would pin the old snapshot
        guard = std::make_unique<Engine::Store::Guard>(engineReader->read());
        return guard->get();
    }

    ActivitySpec newActivity(const char *name) {
        ActivitySpec a;
        a.name = name;
        a.startup = {haAction("dev_lg_tv", "media_player.turn_on")};
        a.bindings = {bind("up:down", {bleKey("key_down", "ARROW_UP")}), bind("up:up", {bleKey("key_up", "ARROW_UP")})};
        return a;
    }

    storage::MemoryStorage mem;
    std::unique_ptr<storage::SlotRecordStore> records;
    FakeAdapters fakes;
    ConfigListener *listener = nullptr;  // set before a boot()

    // Both slot files of the config record, to check nothing was written.
    std::string slots() { return mem.files()["config.a"] + "|" + mem.files()["config.b"]; }
    std::unique_ptr<Engine::Store> store;
    std::unique_ptr<Engine::Store::Reader> engineReader;
    std::unique_ptr<Engine::Store::Guard> guard;
    std::unique_ptr<ConfigService> service;
    LoadReport report;

    void TearDown() override {
        guard.reset();
        engineReader.reset();
    }
};

}  // namespace

TEST_F(ServiceTest, FirstBootUsesAndSavesDefaults) {
    EXPECT_EQ(report.state, ConfigState::Fresh);
    EXPECT_EQ(report.reason, ErrorCode::Ok);
    EXPECT_TRUE(report.saved);
    EXPECT_EQ(report.generation, 1u);
    EXPECT_EQ(service->state(), ConfigState::Ok);
    EXPECT_EQ(mem.files().count("config.a"), 1u);
    for (const auto &a : service->config().activities) EXPECT_EQ(a.revision, 1u);
    EXPECT_NE(service->findIntegration("ble"), nullptr);  // built-ins always present
    ASSERT_NE(published(), nullptr);
    EXPECT_EQ(published()->activities.size(), 2u);
}

TEST_F(ServiceTest, ConfigSurvivesReboot) {
    const Outcome created = service->createActivity(newActivity("Movie Night"));
    ASSERT_TRUE(created.ok()) << errorCodeName(created.code);
    EXPECT_EQ(created.id, "act_movie_night");
    EXPECT_EQ(created.revision, 2u);  // the generation of its last change
    const uint32_t revision = service->config().revision;

    guard.reset();
    service.reset();
    boot();
    EXPECT_EQ(report.state, ConfigState::Ok);
    EXPECT_FALSE(report.saved);
    EXPECT_EQ(report.generation, 2u);
    ASSERT_NE(service->findActivity("act_movie_night"), nullptr);
    EXPECT_EQ(service->config().revision, revision);
    EXPECT_GE(published()->findActivity("act_movie_night"), 0);
}

TEST_F(ServiceTest, ACorruptNewestSlotFallsBackToTheOlderOneUntouched) {
    ASSERT_TRUE(service->createActivity(newActivity("Movie Night")).ok());  // generation 2: config.b
    ASSERT_TRUE(service->createActivity(newActivity("Late Show")).ok());    // generation 3: config.a
    mem.files()["config.a"][40] ^= 0x01;  // flip a payload bit in the newest slot
    const std::string both = slots();
    guard.reset();
    service.reset();
    boot();
    EXPECT_EQ(report.state, ConfigState::Ok);
    EXPECT_EQ(report.generation, 2u);
    EXPECT_NE(service->findActivity("act_movie_night"), nullptr);
    EXPECT_EQ(service->findActivity("act_late_show"), nullptr);
    EXPECT_EQ(slots(), both);  // load never writes
    ASSERT_TRUE(service->createActivity(newActivity("Again")).ok());
    EXPECT_EQ(service->generation(), 3u);  // the next after the newest intact; it overwrites the corrupt slot
    EXPECT_NE(mem.files()["config.a"], "");
}

TEST_F(ServiceTest, IdsAreSlugsAndUnique) {
    EXPECT_EQ(service->createActivity(newActivity("Watch Apple TV!")).id, "act_watch_apple_tv_2");
    EXPECT_EQ(service->createActivity(newActivity("!!!")).id, "act_activity");
    const std::string longName(60, 'x');
    EXPECT_EQ(service->createActivity(newActivity(longName.c_str())).id, "act_" + std::string(28, 'x'));
    EXPECT_EQ(service->createActivity(newActivity(longName.c_str())).id, "act_" + std::string(26, 'x') + "_2");
    EXPECT_EQ(service->createActivity(newActivity("")).code, ErrorCode::ValidationFailed);  // name required
}

TEST_F(ServiceTest, ReplaceUsesOptimisticRevisions) {
    ActivitySpec edit = *service->findActivity("act_play_xbox");
    edit.name = "Xbox";
    const Outcome stale = service->replaceActivity("act_play_xbox", edit, 7);
    EXPECT_EQ(stale.code, ErrorCode::RevisionConflict);
    EXPECT_EQ(stale.currentRevision, 1u);

    const Outcome ok = service->replaceActivity("act_play_xbox", edit, 1);
    ASSERT_TRUE(ok.ok());
    EXPECT_EQ(ok.revision, 2u);
    EXPECT_EQ(service->findActivity("act_watch_apple_tv")->revision, 1u);  // untouched objects keep theirs
    EXPECT_EQ(service->findActivity("act_play_xbox")->name, "Xbox");
    EXPECT_EQ(service->replaceActivity("act_nope", edit, 1).code, ErrorCode::NotFound);
    edit.id = "act_other";
    EXPECT_TRUE(service->replaceActivity("act_play_xbox", edit, 2).issues.has(ErrorCode::InvalidId, "/id"));
}

TEST_F(ServiceTest, InvalidEditChangesNothingAndPathsAreRootedAtTheBody) {
    const std::string before = slots();
    const uint32_t serialBefore = store->serial();
    ActivitySpec edit = *service->findActivity("act_watch_apple_tv");
    edit.startup.push_back(haAction("dev_ghost", "media_player.turn_on"));
    const Outcome o = service->replaceActivity("act_watch_apple_tv", edit, 1);
    EXPECT_EQ(o.code, ErrorCode::ValidationFailed);
    EXPECT_TRUE(o.issues.has(ErrorCode::UnknownDevice, "/startup/5/device_id"));
    EXPECT_EQ(slots(), before);
    EXPECT_EQ(store->serial(), serialBefore);
    EXPECT_EQ(service->findActivity("act_watch_apple_tv")->revision, 1u);
}

TEST_F(ServiceTest, StorageFailureRollsBackTheEngineToo) {
    mem.crashAfter(0);
    const Outcome o = service->createActivity(newActivity("Movie Night"));
    mem.reboot();
    EXPECT_EQ(o.code, ErrorCode::StorageFailed);
    EXPECT_EQ(service->findActivity("act_movie_night"), nullptr);
    EXPECT_EQ(published()->findActivity("act_movie_night"), -1);  // never published
}

TEST_F(ServiceTest, DeleteActivityRefusedWhileActivatedByAButton) {
    const Outcome refused = service->deleteActivity("act_play_xbox", 1);
    EXPECT_EQ(refused.code, ErrorCode::Referenced);
    ASSERT_EQ(refused.referrers.size(), 1u);
    EXPECT_EQ(refused.referrers[0].activityId, "");  // global bindings
    EXPECT_EQ(refused.referrers[0].path, "/bindings/music:tap/0");
    EXPECT_EQ(refused.referrers[0].activityName, "");

    auto globals = service->config().globalBindings;
    globals.erase(globals.begin() + 3);  // music:tap
    ASSERT_TRUE(service->replaceGlobalBindings(globals, 1).ok());
    EXPECT_EQ(service->deleteActivity("act_play_xbox", 2).code, ErrorCode::RevisionConflict);
    EXPECT_TRUE(service->deleteActivity("act_play_xbox", 1).ok());
    EXPECT_EQ(published()->findActivity("act_play_xbox"), -1);
}

TEST_F(ServiceTest, GlobalBindingIssuesAreRootedAtTheBody) {
    auto globals = service->config().globalBindings;
    globals.push_back(bind("red:tap", {activateAction("act_nope")}));
    const Outcome o = service->replaceGlobalBindings(globals, 1);
    EXPECT_EQ(o.code, ErrorCode::ValidationFailed);
    EXPECT_TRUE(o.issues.has(ErrorCode::UnknownActivity, "/bindings/red:tap/0/params/activity_id"));
    EXPECT_EQ(service->replaceGlobalBindings(globals, 9).currentRevision, 1u);
}

TEST_F(ServiceTest, DeviceLifecycle) {
    DeviceSpec tv = haDevice("", "Bedroom TV", "tv", "media_player.family_room_lg_tv");
    const Outcome created = service->createDevice(tv);
    ASSERT_TRUE(created.ok()) << (created.issues.ok() ? errorCodeName(created.code) : created.issues.issues()[0].path);
    EXPECT_EQ(created.id, "dev_bedroom_tv");

    DeviceUpdate rename;
    rename.name = "Bedroom LG";
    rename.revision = created.revision;
    const Outcome renamed = service->updateDevice("dev_bedroom_tv", rename);
    EXPECT_EQ(renamed.revision, created.revision + 1);
    EXPECT_EQ(service->updateDevice("dev_bedroom_tv", rename).code, ErrorCode::RevisionConflict);

    const Outcome inUse = service->deleteDevice("dev_denon", 1);
    EXPECT_EQ(inUse.code, ErrorCode::DeviceInUse);
    EXPECT_GE(inUse.referrers.size(), 4u);
    EXPECT_EQ(inUse.referrers[0].activityId, "act_watch_apple_tv");
    EXPECT_EQ(inUse.referrers[0].path, "/startup/1");
    EXPECT_EQ(inUse.referrers[0].activityName, "Watch Apple TV");

    EXPECT_TRUE(service->deleteDevice("dev_bedroom_tv", renamed.revision).ok());
    EXPECT_EQ(service->findDevice("dev_bedroom_tv"), nullptr);

    DeviceSpec bad = haDevice("", "Ghost", "tv", "");
    EXPECT_TRUE(service->createDevice(bad).issues.has(ErrorCode::Required, "/provider"));
}

TEST_F(ServiceTest, Limits) {
    for (size_t i = service->config().activities.size(); i < limits::kMaxActivities; i++) {
        ASSERT_TRUE(service->createActivity(newActivity("A")).ok()) << i;
    }
    EXPECT_EQ(service->createActivity(newActivity("A")).code, ErrorCode::LimitReached);
}

TEST_F(ServiceTest, IntegrationSettings) {
    IntegrationSpec ha = *service->findIntegration("ha_main");
    ha.url = "https://ha.example:8123";
    EXPECT_EQ(service->replaceIntegration("ha_main", ha, 1).revision, 2u);
    EXPECT_EQ(service->findIntegration("ha_main")->url, "https://ha.example:8123");
    ha.type = "ble_hid";
    EXPECT_TRUE(service->replaceIntegration("ha_main", ha, 2).issues.has(ErrorCode::IntegrationMismatch, "/type"));
}

TEST_F(ServiceTest, CompileCommand) {
    CompiledCommand cmd;
    Outcome o = service->compileCommand("dev_denon", "media_player.volume_set", {param("level", 0.25)}, &cmd);
    ASSERT_TRUE(o.ok());
    EXPECT_EQ(cmd.deviceId, "dev_denon");
    EXPECT_EQ(cmd.providerRef, "media_player.denon");
    EXPECT_FLOAT_EQ(cmd.action.params[0].number, 0.25f);
    EXPECT_EQ(cmd.action.paramsPresent, 1);

    EXPECT_EQ(service->compileCommand("dev_nope", "x", {}, &cmd).code, ErrorCode::NotFound);
    EXPECT_TRUE(service->compileCommand("dev_denon", "homeassistant.restart", {}, &cmd)
                    .issues.has(ErrorCode::UnknownCommand, "/command_id"));
    EXPECT_TRUE(service->compileCommand("dev_denon", "media_player.volume_set", {param("level", 2.0)}, &cmd)
                    .issues.has(ErrorCode::ParamOutOfRange, "/params/level"));
}

TEST(ServiceFlash, NoWritesOnButtonPressOrActivitySwitch) {
    SimHub hub;
    hub.boot(fixtureConfig());
    auto &mem = static_cast<storage::MemoryStorage &>(hub.storage());
    const int writes = mem.mutations();
    hub.activate("act_watch_apple_tv");
    hub.advance(1500);
    hub.tap(Button::Up);
    hub.tap(Button::Music);
    hub.advance(3000);
    hub.tap(Button::Off);
    EXPECT_EQ(mem.mutations(), writes);
}

TEST_F(ServiceTest, ValidateADraftWithoutAnId) {
    ActivitySpec draft = newActivity("Movie Night");
    EXPECT_TRUE(service->validateActivity(draft).ok());
    draft.startup.push_back(bleKey("key_tap", "NOPE"));
    EXPECT_TRUE(service->validateActivity(draft).has(ErrorCode::InvalidEnumValue, "/startup/1/params/key"));
    EXPECT_EQ(service->findActivity("act_movie_night"), nullptr);  // nothing saved
}

// ---- state follows the medium ---------------------------------------------

TEST_F(ServiceTest, StateFollowsStorageHealthLive) {
    const uint32_t generation = service->generation();
    const std::string before = slots();
    for (storage::Health down : {storage::Health::Unavailable, storage::Health::ReadOnly}) {
        mem.setHealth(down);
        EXPECT_EQ(service->state(), ConfigState::StorageUnavailable);
        EXPECT_EQ(service->createActivity(newActivity("During Outage")).code, ErrorCode::StorageFailed);
        EXPECT_NE(published()->findActivity("act_watch_apple_tv"), -1);  // what's loaded keeps running
        mem.setHealth(storage::Health::Available);
        EXPECT_EQ(service->state(), ConfigState::Ok);
    }
    EXPECT_EQ(service->generation(), generation);
    EXPECT_EQ(slots(), before);  // nothing written
    EXPECT_TRUE(service->createActivity(newActivity("After Outage")).ok());
}

TEST_F(ServiceTest, ALoadThatFailedOnStorageRecoversWhenItComesBack) {
    ASSERT_TRUE(service->createActivity(newActivity("Movie Night")).ok());
    const uint32_t generation = service->generation();
    const std::string before = slots();
    mem.setHealth(storage::Health::Unavailable);
    guard.reset();
    service.reset();
    boot();
    EXPECT_EQ(report.state, ConfigState::StorageUnavailable);
    EXPECT_EQ(service->refresh(0), ConfigState::StorageUnavailable);  // still down: nothing happens
    EXPECT_EQ(published()->activities.size(), 0u);                  // nothing runs
    mem.setHealth(storage::Health::Available);
    EXPECT_EQ(service->refresh(1), ConfigState::Ok);
    EXPECT_EQ(service->generation(), generation);
    EXPECT_NE(service->findActivity("act_movie_night"), nullptr);
    EXPECT_GE(published()->findActivity("act_movie_night"), 0);
    EXPECT_EQ(slots(), before);  // the retried load wrote nothing
}

TEST_F(ServiceTest, ARetriedLoadNeverTakesAnUnreadableSlotForEmpty) {
    const std::string before = slots();
    mem.failReads("config.a");  // generation 1 lives in slot a
    guard.reset();
    service.reset();
    boot();
    EXPECT_EQ(report.state, ConfigState::StorageUnavailable);
    EXPECT_EQ(service->refresh(0), ConfigState::StorageUnavailable);  // healthy medium, but still unreadable
    EXPECT_EQ(service->createActivity(newActivity("Nope")).code, ErrorCode::StorageFailed);
    EXPECT_EQ(slots(), before);  // never "fresh", never saved over
    mem.failReads("config.a", false);
    EXPECT_EQ(service->refresh(1000), ConfigState::Ok);
    EXPECT_EQ(slots(), before);
}

TEST_F(ServiceTest, BlockedStaysBlockedWhateverTheMedium) {
    std::string newer = config::serializeConfig(fixtureConfig());
    newer.replace(newer.find("\"schema_version\":2"), 18, "\"schema_version\":9");
    ASSERT_EQ(records->save(ConfigService::kRecordName, 50, newer), ErrorCode::Ok);
    guard.reset();
    service.reset();
    boot();
    ASSERT_EQ(service->state(), ConfigState::Blocked);
    mem.setHealth(storage::Health::Unavailable);
    EXPECT_EQ(service->state(), ConfigState::Blocked);
    mem.setHealth(storage::Health::Available);
    EXPECT_EQ(service->refresh(0), ConfigState::Blocked);
}

// ---- one pipeline (design part 5): a dry run is the real restore minus the save ----

namespace {

std::string describe(const Outcome &o) {
    std::string s = std::string(errorCodeName(o.code)) + " rev=" + std::to_string(o.revision);
    for (const ValidationIssue &i : o.issues.issues()) s += " " + std::string(errorCodeName(i.code)) + "@" + i.path;
    return s;
}

}  // namespace

TEST_F(ServiceTest, ADryRunRestoreMatchesTheRealOneAndWritesNothing) {
    ConfigSpec backup = fixtureConfig();
    auto &ints = backup.integrations;
    ints.erase(std::remove_if(ints.begin(), ints.end(), [](const IntegrationSpec &i) { return i.id == "ble"; }),
               ints.end());  // an older backup without the built-in
    const std::string before = slots();
    const int mutations = mem.mutations();
    const uint32_t serial = published() != nullptr ? guard->serial() : 0;
    const Outcome dry = service->restore(backup, /*dryRun=*/true);
    EXPECT_TRUE(dry.ok()) << describe(dry);
    EXPECT_EQ(slots(), before);
    EXPECT_EQ(mem.mutations(), mutations);
    published();
    EXPECT_EQ(guard->serial(), serial);  // nothing published either

    const Outcome real = service->restore(backup);
    EXPECT_EQ(describe(real), describe(dry));
    EXPECT_NE(service->findIntegration("ble"), nullptr);
    EXPECT_NE(slots(), before);
}

TEST_F(ServiceTest, ADryRunRestoreFailsExactlyLikeTheRealOne) {
    ConfigSpec backup = fixtureConfig();
    for (size_t n = backup.activities.size(); n <= limits::kMaxActivities; n++) {
        ActivitySpec extra = backup.activities[1];
        extra.id = "act_extra_" + std::to_string(n);
        backup.activities.push_back(extra);
    }
    const std::string before = slots();
    const Outcome dry = service->restore(backup, /*dryRun=*/true);
    const Outcome real = service->restore(backup);
    EXPECT_FALSE(dry.ok());
    EXPECT_EQ(describe(real), describe(dry));
    EXPECT_EQ(slots(), before);
}

TEST_F(ServiceTest, ADryRunRestoreWorksWhileBlocked) {
    std::string newer = config::serializeConfig(fixtureConfig());
    newer.replace(newer.find("\"schema_version\":2"), 18, "\"schema_version\":9");
    ASSERT_EQ(records->save(ConfigService::kRecordName, 50, newer), ErrorCode::Ok);
    guard.reset();
    service.reset();
    boot();
    ASSERT_EQ(service->state(), ConfigState::Blocked);
    const std::string before = slots();
    const Outcome dry = service->restore(fixtureConfig(), true);
    EXPECT_TRUE(dry.ok()) << describe(dry);
    EXPECT_EQ(dry.revision, 51u);
    EXPECT_EQ(slots(), before);
    EXPECT_EQ(service->state(), ConfigState::Blocked);
    const Outcome real = service->restore(fixtureConfig());
    EXPECT_EQ(describe(real), describe(dry));
    EXPECT_EQ(service->state(), ConfigState::Ok);
}

TEST_F(ServiceTest, EditsCheckEditableBeforeTheRevision) {
    std::string newer = config::serializeConfig(fixtureConfig());
    newer.replace(newer.find("\"schema_version\":2"), 18, "\"schema_version\":9");
    ASSERT_EQ(records->save(ConfigService::kRecordName, 50, newer), ErrorCode::Ok);
    guard.reset();
    service.reset();
    boot();
    ASSERT_EQ(service->state(), ConfigState::Blocked);
    // A stale (or any) revision: Blocked wins, not RevisionConflict.
    EXPECT_EQ(service->replaceGlobalBindings({}, 12345).code, ErrorCode::ConfigBlocked);
    EXPECT_EQ(service->replaceActivity("act_watch_apple_tv", newActivity("X"), 12345).code, ErrorCode::ConfigBlocked);
    EXPECT_EQ(service->deleteDevice("dev_lg_tv", 12345).code, ErrorCode::ConfigBlocked);

    // Storage gone: StorageFailed, not RevisionConflict.
    guard.reset();
    service.reset();
    mem.files().clear();
    boot();
    mem.setHealth(storage::Health::Unavailable);
    EXPECT_EQ(service->replaceGlobalBindings({}, 12345).code, ErrorCode::StorageFailed);
}

// ---- refresh() backoff (review #9) ----

TEST_F(ServiceTest, RefreshBacksOffWhileASlotStaysUnreadable) {
    mem.failReads("config.a");
    guard.reset();
    service.reset();
    boot();
    ASSERT_EQ(report.state, ConfigState::StorageUnavailable);
    const uint32_t serial = (published(), guard->serial());

    // Retry times on an Available medium: at once, then 1 s, 2 s, 4 s ...
    // capped at 60 s. Count the loads by the slot reads they make.
    std::vector<uint64_t> retries;
    for (uint64_t now = 0; now <= 300000; now += 100) {
        const int reads = mem.reads();
        EXPECT_EQ(service->refresh(now), ConfigState::StorageUnavailable);
        if (mem.reads() != reads) retries.push_back(now);
    }
    const std::vector<uint64_t> expected = {0, 1000, 3000, 7000, 15000, 31000, 63000, 123000, 183000, 243000};
    EXPECT_EQ(retries, expected);
    published();
    EXPECT_EQ(guard->serial(), serial);  // the empty snapshot isn't republished on every retry

    // A health transition retries at once, whatever the backoff.
    mem.setHealth(storage::Health::ReadOnly);
    int reads = mem.reads();
    service->refresh(300100);
    EXPECT_EQ(mem.reads(), reads);  // not Available: no retry
    mem.setHealth(storage::Health::Available);
    mem.failReads("config.a", false);
    EXPECT_EQ(service->refresh(300200), ConfigState::Ok);
}

// ---- absence needs proof (design part 6) ----

// Bluetooth hosts follow the saved config: a host whose device isn't in it
// is forgotten at boot and after every commit, so a delete doesn't need
// forget() to succeed. Nothing is forgotten while Blocked.
TEST_F(ServiceTest, BluetoothHostsFollowTheSavedConfig) {
    sim::SimulatedBleHosts &hosts = fakes.bleHosts;
    integrations::BleHostReconciler reconciler(hosts);
    listener = &reconciler;
    const auto bonded = [&](const char *device) { return hosts.status(device).bonded; };

    // Boot: an orphan (a device no longer in the config) goes; the Apple TV stays.
    hosts.bond("dev_apple_tv", "Apple TV");
    hosts.bond("dev_gone", "Old TV");
    guard.reset();
    service.reset();
    boot();
    ASSERT_EQ(report.state, ConfigState::Ok);
    EXPECT_TRUE(bonded("dev_apple_tv"));
    EXPECT_FALSE(bonded("dev_gone"));

    // A delete drops its host, and an open window for a device that's gone closes.
    const Outcome created = service->createDevice(bleDevice("", "Shield", "streamer"));
    ASSERT_TRUE(created.ok());
    hosts.bond(created.id, "SHIELD");
    ASSERT_EQ(hosts.startPairing(created.id, integrations::kBlePairingMinWindowMs),
              integrations::BleHostResult::Accepted);
    ASSERT_TRUE(service->deleteDevice(created.id, created.revision).ok());
    EXPECT_FALSE(bonded(created.id.c_str()));
    EXPECT_FALSE(hosts.status(created.id).pairing);
    EXPECT_TRUE(bonded("dev_apple_tv"));

    // A restore without the Apple TV drops it.
    ASSERT_TRUE(service->restore(fixtureConfigWithoutBluetooth()).ok());
    EXPECT_FALSE(bonded("dev_apple_tv"));

    // Blocked: no proof of what's gone, so nothing is forgotten.
    hosts.bond("dev_apple_tv", "Apple TV");
    mem.files()["config.a"] = "garbage";
    mem.files()["config.b"] = "garbage";
    guard.reset();
    service.reset();
    boot();
    ASSERT_EQ(report.state, ConfigState::Blocked);
    EXPECT_TRUE(bonded("dev_apple_tv"));
}

TEST_F(ServiceTest, ACorruptRecordBlocksAndIsNeverOverwrittenWithDefaults) {
    std::string newerFormat = storage::encodeSlot(7, "{}");
    newerFormat[4] = 2;  // an unknown slot format
    for (const std::string &damage : {std::string("garbage"), newerFormat}) {
        mem.files().clear();
        mem.files()["config.a"] = damage;
        mem.files()["config.b"] = damage;
        const std::string before = slots();
        const int mutations = mem.mutations();
        guard.reset();
        service.reset();
        boot();
        EXPECT_EQ(report.state, ConfigState::Blocked);
        EXPECT_EQ(report.reason, ErrorCode::Corrupt);
        EXPECT_STREQ(errorCodeName(report.reason), "corrupt");
        EXPECT_FALSE(report.saved);
        EXPECT_EQ(slots(), before);
        EXPECT_EQ(mem.mutations(), mutations);
        EXPECT_EQ(published()->activities.size(), 0u);  // nothing runs
        EXPECT_EQ(service->createActivity(newActivity("X")).code, ErrorCode::ConfigBlocked);
        std::string payload;
        EXPECT_EQ(service->blockedRecord(&payload), ErrorCode::Corrupt);  // nothing intact to export
        // A restore is the way out.
        ASSERT_TRUE(service->restore(fixtureConfig()).ok());
        EXPECT_EQ(service->state(), ConfigState::Ok);
    }
}

TEST_F(ServiceTest, InvalidatedAndAbsentSlotsAreAFreshStart) {
    mem.files().clear();
    mem.files()["config.a"] = storage::invalidatedSlot();
    guard.reset();
    service.reset();
    boot();
    EXPECT_EQ(report.state, ConfigState::Fresh);
    EXPECT_TRUE(report.saved);
}

TEST_F(ServiceTest, ABlockedRecordCanBeExportedAsIs) {
    std::string payload;
    EXPECT_EQ(service->blockedRecord(&payload), ErrorCode::NotFound);  // not blocked
    std::string newer = config::serializeConfig(fixtureConfig());
    newer.replace(newer.find("\"schema_version\":2"), 18, "\"schema_version\":9");
    ASSERT_EQ(records->save(ConfigService::kRecordName, 50, newer), ErrorCode::Ok);
    guard.reset();
    service.reset();
    boot();
    ASSERT_EQ(service->state(), ConfigState::Blocked);
    uint64_t generation = 0;
    ASSERT_EQ(service->blockedRecord(&payload, &generation), ErrorCode::Ok);
    EXPECT_EQ(payload, newer);  // the saved bytes, not the empty runtime config
    EXPECT_EQ(generation, 50u);
}

// The defaults can't be saved (read-only medium): they run unsaved, built by
// the real pipeline (normalized, stamped, round-tripped).
TEST_F(ServiceTest, UnsavedDefaultsRunThroughThePipeline) {
    mem.files().clear();
    mem.setHealth(storage::Health::ReadOnly);
    guard.reset();
    service.reset();
    ConfigSpec defaults = fixtureConfig();
    auto &ints = defaults.integrations;
    ints.erase(std::remove_if(ints.begin(), ints.end(), [](const IntegrationSpec &i) { return i.id == "ble"; }),
               ints.end());
    engineReader = std::make_unique<Engine::Store::Reader>();
    store = std::make_unique<Engine::Store>();
    store->attach(*engineReader);
    records = std::make_unique<storage::SlotRecordStore>(mem, kMaxConfigFileBytes);
    service = std::make_unique<ConfigService>(*records, fakes.set, *store);
    report = service->load(defaults);
    EXPECT_EQ(report.state, ConfigState::Fresh);
    EXPECT_EQ(report.reason, ErrorCode::StorageFailed);
    EXPECT_FALSE(report.saved);
    EXPECT_TRUE(mem.files().empty());
    EXPECT_EQ(service->config().revision, 1u);
    EXPECT_NE(service->findIntegration("ble"), nullptr);  // built-ins added, as in a commit
    EXPECT_EQ(published()->activities.size(), defaults.activities.size());
    ConfigSpec reparsed;
    ASSERT_EQ(config::parseConfig(config::serializeConfig(service->config()), &reparsed).code, ErrorCode::Ok);
    EXPECT_EQ(config::serializeConfig(reparsed), config::serializeConfig(service->config()));  // the round-tripped form
}
