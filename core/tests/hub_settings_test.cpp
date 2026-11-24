#include <utility>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "hubbo/config/hub.h"
#include "hubbo/storage/record_store.h"
#include "hubbo/storage/state_store.h"
#include "hubbo/storage/storage.h"

namespace hubbo::config {
namespace {

constexpr const char *kDefault = "Hubbo 3F2A";

TEST(HubName, HostnameIsADnsSafeSlug) {
    EXPECT_EQ(hostnameFor("Hubbo"), "hubbo");
    EXPECT_EQ(hostnameFor("Living Room"), "living-room");
    EXPECT_EQ(hostnameFor("  Den -- TV!! 2 "), "den-tv-2");
    EXPECT_EQ(hostnameFor("Café Hub"), "caf-hub");  // non-ASCII bytes are a run
    EXPECT_EQ(hostnameFor("\xF0\x9F\x93\xBA\xF0\x9F\x8E\xAE"), "hubbo");  // all emoji: nothing left
    EXPECT_EQ(hostnameFor("---"), "hubbo");
    EXPECT_EQ(hostnameFor(""), "hubbo");
    const std::string longName(40, 'a');
    EXPECT_EQ(hostnameFor(longName + " " + longName).size(), kHostnameMaxLength);
    // Cut at 63 right after a separator: no trailing '-'.
    const std::string cut = hostnameFor(std::string(62, 'b') + " c");
    EXPECT_EQ(cut, std::string(62, 'b'));
    for (const std::string &name : {std::string("Ünïcödé Ñame"), std::string("A_B.C"), longName}) {
        const std::string h = hostnameFor(name);
        EXPECT_LE(h.size(), kHostnameMaxLength);
        EXPECT_NE(h.front(), '-');
        EXPECT_NE(h.back(), '-');
        for (const char c : h) EXPECT_TRUE((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') << h;
    }
}

TEST(HubName, NamesAreTrimmedAndChecked) {
    EXPECT_EQ(trimHubName("  Den  "), "Den");
    EXPECT_EQ(checkHubName("Hubbo"), ErrorCode::Ok);
    EXPECT_EQ(checkHubName("Living Room 📺"), ErrorCode::Ok);
    EXPECT_EQ(checkHubName(""), ErrorCode::Required);
    EXPECT_EQ(checkHubName(std::string(32, 'x')), ErrorCode::Ok);
    EXPECT_EQ(checkHubName(std::string(33, 'x')), ErrorCode::TooLong);
    std::string emoji;
    for (int i = 0; i < 32; i++) emoji += "\xF0\x9F\x93\xBA";  // 32 code points, 128 bytes
    EXPECT_EQ(checkHubName(emoji), ErrorCode::Ok);
    EXPECT_EQ(checkHubName(emoji + "x"), ErrorCode::TooLong);
    EXPECT_EQ(checkHubName("a\tb"), ErrorCode::InvalidName);
    EXPECT_EQ(checkHubName("a\x7F"), ErrorCode::InvalidName);
    EXPECT_EQ(checkHubName("a\xC2\x85"), ErrorCode::InvalidName);  // U+0085, a C1 control
    EXPECT_EQ(checkHubName("a\xFF"), ErrorCode::InvalidName);      // not UTF-8
    EXPECT_EQ(checkHubName("a\xC0\xAF"), ErrorCode::InvalidName);  // overlong
    EXPECT_EQ(checkHubName(" Den"), ErrorCode::InvalidName);       // untrimmed
}

TEST(HubSettings, LoadsRenamesAndTellsThePlatform) {
    storage::MemoryStateStore store;
    HubSettings hub(store, kDefault);
    EXPECT_EQ(hub.load(), ErrorCode::NotFound);
    EXPECT_EQ(hub.identity().name, kDefault);
    EXPECT_EQ(hub.identity().hostname, "hubbo-3f2a");

    std::vector<HubIdentity> applied;
    hub.onChanged([&](const HubIdentity &identity) { applied.push_back(identity); });
    HubIdentity out;
    EXPECT_EQ(hub.rename(" Living Room ", &out), ErrorCode::Ok);
    EXPECT_EQ(out.name, "Living Room");
    EXPECT_EQ(out.hostname, "living-room");
    ASSERT_EQ(applied.size(), 1u);
    EXPECT_EQ(applied[0].name, "Living Room");

    // The same name again (after trimming): Ok, no write, nobody told.
    const int writes = store.writes();
    EXPECT_EQ(hub.rename("Living Room  ", &out), ErrorCode::Ok);
    EXPECT_EQ(out.hostname, "living-room");
    EXPECT_EQ(store.writes(), writes);
    EXPECT_EQ(applied.size(), 1u);

    // Refused names and failed saves change nothing and tell nobody.
    EXPECT_EQ(hub.rename("   "), ErrorCode::Required);
    store.failWrites = true;
    EXPECT_EQ(hub.rename("Den"), ErrorCode::StorageFailed);
    store.failWrites = false;
    EXPECT_EQ(hub.identity().name, "Living Room");
    EXPECT_EQ(applied.size(), 1u);

    // The next boot reads it back.
    HubSettings again(store, kDefault);
    EXPECT_EQ(again.load(), ErrorCode::Ok);
    EXPECT_EQ(again.identity().hostname, "living-room");
}

TEST(HubSettings, AnUnreadableNameIsTheDefaultAndStaysSaved) {
    storage::MemoryStateStore store;
    ASSERT_TRUE(store.set(kHubNameKey, "Den"));
    store.failReads = true;
    HubSettings hub(store, kDefault);
    EXPECT_EQ(hub.load(), ErrorCode::StorageFailed);
    EXPECT_EQ(hub.identity().name, kDefault);
    store.failReads = false;
    std::string saved;
    EXPECT_EQ(store.get(kHubNameKey, &saved), ErrorCode::Ok);
    EXPECT_EQ(saved, "Den");  // nothing written over it
    // Asking for the default it shows still saves it: what's saved is "Den".
    EXPECT_EQ(hub.rename(kDefault), ErrorCode::Ok);
    EXPECT_EQ(store.get(kHubNameKey, &saved), ErrorCode::Ok);
    EXPECT_EQ(saved, kDefault);

    ASSERT_TRUE(store.set(kHubNameKey, "bad\x01"));
    EXPECT_EQ(hub.load(), ErrorCode::Corrupt);
    EXPECT_EQ(hub.identity().name, kDefault);
}

TEST(HubSettings, ForgetRemovesTheSavedName) {
    storage::MemoryStorage disk;
    storage::SlotRecordStore records(disk, 4096);
    storage::RecordStateStore recordStore(records);
    storage::MemoryStateStore memoryStore;
    for (storage::StateStore *store : {static_cast<storage::StateStore *>(&recordStore),
                                       static_cast<storage::StateStore *>(&memoryStore)}) {
        HubSettings hub(*store, kDefault);
        ASSERT_EQ(hub.rename("Den"), ErrorCode::Ok);
        EXPECT_EQ(hub.forget(), ErrorCode::Ok);
        EXPECT_EQ(hub.identity().name, kDefault);
        std::string saved;
        EXPECT_EQ(store->get(kHubNameKey, &saved), ErrorCode::NotFound);
        EXPECT_EQ(hub.forget(), ErrorCode::Ok);  // nothing saved: still Ok
        HubSettings again(*store, kDefault);
        EXPECT_EQ(again.load(), ErrorCode::NotFound);
    }
    EXPECT_EQ(disk.files().count("hub_name.a") + disk.files().count("hub_name.b"), 0u);
    memoryStore.failWrites = true;
    HubSettings failing(memoryStore, kDefault);
    EXPECT_EQ(failing.forget(), ErrorCode::StorageFailed);
}

TEST(HubSettings, TheNameFitsTheRecordStateStore) {
    storage::MemoryStorage disk;
    storage::SlotRecordStore records(disk, 4096);
    storage::RecordStateStore store(records);
    HubSettings hub(store, kDefault);
    std::string emoji;
    for (int i = 0; i < 32; i++) emoji += "\xF0\x9F\x93\xBA";
    ASSERT_EQ(hub.rename(emoji), ErrorCode::Ok);
    HubSettings again(store, kDefault);
    EXPECT_EQ(again.load(), ErrorCode::Ok);
    EXPECT_EQ(again.identity().name, emoji);
}

TEST(HubSettings, TheDefaultNameComesFromTheMac) {
    const uint8_t mac[6] = {0x24, 0x0a, 0xc4, 0x12, 0x3f, 0x2a};
    EXPECT_EQ(defaultHubName(mac), "Hubbo 3F2A");
    const uint8_t low[6] = {0, 0, 0, 0, 0x00, 0x0b};
    EXPECT_EQ(defaultHubName(low), "Hubbo 000B");
    EXPECT_EQ(checkHubName(defaultHubName(mac)), ErrorCode::Ok);
    EXPECT_EQ(hostnameFor(defaultHubName(mac)), "hubbo-3f2a");
    // A hub that kept plain "Hubbo" saved keeps it: no migration.
    storage::MemoryStateStore store;
    ASSERT_TRUE(store.set(kHubNameKey, "Hubbo"));
    HubSettings hub(store, defaultHubName(mac));
    EXPECT_EQ(hub.load(), ErrorCode::Ok);
    EXPECT_EQ(hub.identity().name, "Hubbo");
}

// What mDNS really advertises is shown; a clash shows as a conflict. It's
// runtime only, and a rename or a reload clears it (the platform reports again).
TEST(HubSettings, TheAdvertisedHostnameIsShownUntilTheNameChanges) {
    storage::MemoryStateStore store;
    HubSettings hub(store, kDefault);
    ASSERT_EQ(hub.rename("Living Room"), ErrorCode::Ok);
    EXPECT_EQ(hub.identity().shownHostname(), "living-room");  // not reported yet
    EXPECT_FALSE(hub.identity().hostnameConflict());
    hub.setAdvertisedHostname("living-room");
    EXPECT_FALSE(hub.identity().hostnameConflict());
    hub.setAdvertisedHostname("living-room-2");
    EXPECT_EQ(hub.identity().hostname, "living-room");
    EXPECT_EQ(hub.identity().shownHostname(), "living-room-2");
    EXPECT_TRUE(hub.identity().hostnameConflict());
    const int writes = store.writes();
    hub.setAdvertisedHostname("living-room-3");
    EXPECT_EQ(store.writes(), writes);  // never saved
    // Renaming to the same name changes nothing: the answer keeps the clash.
    HubIdentity same;
    ASSERT_EQ(hub.rename("Living Room ", &same), ErrorCode::Ok);
    EXPECT_EQ(same.shownHostname(), "living-room-3");
    EXPECT_TRUE(same.hostnameConflict());
    EXPECT_EQ(hub.identity().advertisedHostname, "living-room-3");

    // The board's mDNS reports during the rename's `changed` (it waits on the
    // mDNS task): that report stays, and a report never waits on a rename.
    std::vector<HubIdentity> applied;
    hub.onChanged([&](const HubIdentity &identity) {
        applied.push_back(identity);
        hub.setAdvertisedHostname("den-2");
    });
    ASSERT_EQ(hub.rename("Den"), ErrorCode::Ok);
    ASSERT_EQ(applied.size(), 1u);
    EXPECT_EQ(applied[0].advertisedHostname, "");
    EXPECT_EQ(hub.identity().shownHostname(), "den-2");
    EXPECT_EQ(hub.load(), ErrorCode::Ok);
    EXPECT_EQ(hub.identity().advertisedHostname, "");
    hub.setAdvertisedHostname("den-2");
    EXPECT_EQ(hub.forget(), ErrorCode::Ok);
    EXPECT_EQ(hub.identity().advertisedHostname, "");
    EXPECT_EQ(hub.identity().shownHostname(), "hubbo-3f2a");
}

TEST(HubSettings, CheckForUpdatesIsOnUnlessTurnedOff) {
    storage::MemoryStateStore store;
    HubSettings hub(store, kDefault);
    EXPECT_EQ(hub.load(), ErrorCode::NotFound);
    EXPECT_TRUE(hub.checkUpdates());  // nothing saved: on
    ASSERT_EQ(hub.setCheckUpdates(false), ErrorCode::Ok);
    EXPECT_FALSE(hub.checkUpdates());
    HubSettings reloaded(store, kDefault);
    reloaded.load();
    EXPECT_FALSE(reloaded.checkUpdates());
    // A reset turns it back on.
    EXPECT_EQ(reloaded.forget(), ErrorCode::Ok);
    EXPECT_TRUE(reloaded.checkUpdates());
    std::string saved;
    EXPECT_EQ(store.get(kCheckUpdatesKey, &saved), ErrorCode::NotFound);
    // A failed save changes nothing.
    store.failWrites = true;
    EXPECT_EQ(reloaded.setCheckUpdates(false), ErrorCode::StorageFailed);
    EXPECT_TRUE(reloaded.checkUpdates());
}

TEST(HubSettings, RadioPowerNamesRoundTrip) {
    const std::pair<RadioPower, const char *> levels[] = {
        {RadioPower::Low, "low"}, {RadioPower::Medium, "medium"}, {RadioPower::High, "high"}, {RadioPower::Max, "max"}};
    for (const auto &[power, name] : levels) {
        EXPECT_STREQ(radioPowerName(power), name);
        RadioPower parsed{};
        ASSERT_TRUE(parseRadioPower(name, &parsed)) << name;
        EXPECT_EQ(parsed, power);
    }
    RadioPower untouched = RadioPower::High;
    for (const char *bad : {"", "MAX", "loud", "max "}) {
        EXPECT_FALSE(parseRadioPower(bad, &untouched)) << bad;
    }
    EXPECT_EQ(untouched, RadioPower::High);
}

TEST(HubSettings, RadioPowerIsMaxUntilSet) {
    storage::MemoryStateStore store;
    HubSettings hub(store, kDefault);
    EXPECT_EQ(hub.radioPower(), RadioPower::Max);  // before load
    hub.load();
    EXPECT_EQ(hub.radioPower(), RadioPower::Max);  // nothing saved
    ASSERT_EQ(hub.setRadioPower(RadioPower::Low), ErrorCode::Ok);
    EXPECT_EQ(hub.radioPower(), RadioPower::Low);
    std::string saved;
    ASSERT_EQ(store.get(kRadioPowerKey, &saved), ErrorCode::Ok);
    EXPECT_EQ(saved, "low");
    HubSettings reloaded(store, kDefault);
    reloaded.load();
    EXPECT_EQ(reloaded.radioPower(), RadioPower::Low);
    // A reset goes back to max and removes what's saved.
    EXPECT_EQ(reloaded.forget(), ErrorCode::Ok);
    EXPECT_EQ(reloaded.radioPower(), RadioPower::Max);
    EXPECT_EQ(store.get(kRadioPowerKey, &saved), ErrorCode::NotFound);
    // A failed save changes nothing.
    store.failWrites = true;
    EXPECT_EQ(reloaded.setRadioPower(RadioPower::Medium), ErrorCode::StorageFailed);
    EXPECT_EQ(reloaded.radioPower(), RadioPower::Max);
}

TEST(HubSettings, UnreadableRadioPowerIsMax) {
    storage::MemoryStateStore store;
    HubSettings hub(store, kDefault);
    ASSERT_EQ(hub.setRadioPower(RadioPower::High), ErrorCode::Ok);
    ASSERT_TRUE(store.set(kRadioPowerKey, "loud"));
    hub.load();
    EXPECT_EQ(hub.radioPower(), RadioPower::Max);
    ASSERT_EQ(hub.setRadioPower(RadioPower::High), ErrorCode::Ok);
    store.failReads = true;
    hub.load();
    EXPECT_EQ(hub.radioPower(), RadioPower::Max);
}

}  // namespace
}  // namespace hubbo::config
