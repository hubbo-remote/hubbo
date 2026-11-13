#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "hubbo/config/hub.h"
#include "hubbo/storage/record_store.h"
#include "hubbo/storage/state_store.h"
#include "hubbo/storage/storage.h"

namespace hubbo::config {
namespace {

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
    HubSettings hub(store);
    EXPECT_EQ(hub.load(), ErrorCode::NotFound);
    EXPECT_EQ(hub.identity().name, "Hubbo");
    EXPECT_EQ(hub.identity().hostname, "hubbo");

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
    HubSettings again(store);
    EXPECT_EQ(again.load(), ErrorCode::Ok);
    EXPECT_EQ(again.identity().hostname, "living-room");
}

TEST(HubSettings, AnUnreadableNameIsTheDefaultAndStaysSaved) {
    storage::MemoryStateStore store;
    ASSERT_TRUE(store.set(kHubNameKey, "Den"));
    store.failReads = true;
    HubSettings hub(store);
    EXPECT_EQ(hub.load(), ErrorCode::StorageFailed);
    EXPECT_EQ(hub.identity().name, "Hubbo");
    store.failReads = false;
    std::string saved;
    EXPECT_EQ(store.get(kHubNameKey, &saved), ErrorCode::Ok);
    EXPECT_EQ(saved, "Den");  // nothing written over it
    // Asking for the default it shows still saves it: what's saved is "Den".
    EXPECT_EQ(hub.rename("Hubbo"), ErrorCode::Ok);
    EXPECT_EQ(store.get(kHubNameKey, &saved), ErrorCode::Ok);
    EXPECT_EQ(saved, "Hubbo");

    ASSERT_TRUE(store.set(kHubNameKey, "bad\x01"));
    EXPECT_EQ(hub.load(), ErrorCode::Corrupt);
    EXPECT_EQ(hub.identity().name, "Hubbo");
}

TEST(HubSettings, ForgetRemovesTheSavedName) {
    storage::MemoryStorage disk;
    storage::SlotRecordStore records(disk, 4096);
    storage::RecordStateStore recordStore(records);
    storage::MemoryStateStore memoryStore;
    for (storage::StateStore *store : {static_cast<storage::StateStore *>(&recordStore),
                                       static_cast<storage::StateStore *>(&memoryStore)}) {
        HubSettings hub(*store);
        ASSERT_EQ(hub.rename("Den"), ErrorCode::Ok);
        EXPECT_EQ(hub.forget(), ErrorCode::Ok);
        EXPECT_EQ(hub.identity().name, "Hubbo");
        std::string saved;
        EXPECT_EQ(store->get(kHubNameKey, &saved), ErrorCode::NotFound);
        EXPECT_EQ(hub.forget(), ErrorCode::Ok);  // nothing saved: still Ok
        HubSettings again(*store);
        EXPECT_EQ(again.load(), ErrorCode::NotFound);
    }
    EXPECT_EQ(disk.files().count("hub_name.a") + disk.files().count("hub_name.b"), 0u);
    memoryStore.failWrites = true;
    HubSettings failing(memoryStore);
    EXPECT_EQ(failing.forget(), ErrorCode::StorageFailed);
}

TEST(HubSettings, TheNameFitsTheRecordStateStore) {
    storage::MemoryStorage disk;
    storage::SlotRecordStore records(disk, 4096);
    storage::RecordStateStore store(records);
    HubSettings hub(store);
    std::string emoji;
    for (int i = 0; i < 32; i++) emoji += "\xF0\x9F\x93\xBA";
    ASSERT_EQ(hub.rename(emoji), ErrorCode::Ok);
    HubSettings again(store);
    EXPECT_EQ(again.load(), ErrorCode::Ok);
    EXPECT_EQ(again.identity().name, emoji);
}

}  // namespace
}  // namespace hubbo::config
