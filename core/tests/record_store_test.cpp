// The record store contract (docs/design/persistence-and-contracts.md,
// section 1). Every RecordStore implementation runs the contract tests; the
// in-memory medium also runs crash injection at every write with every torn
// length. A platform implementation (e.g. firmware NVS) adds a harness here.
#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <random>

#include <unistd.h>

#include "hubbo/storage/record_store.h"
#include "hubbo/storage/state_store.h"

using namespace hubbo;
using namespace hubbo::storage;

namespace {

// One implementation under test. `reopen` simulates a reboot: a fresh store
// object over the same medium.
struct Harness {
    virtual ~Harness() = default;
    virtual RecordStore &store() = 0;
    virtual void reopen() = 0;
    // Make reads of one slot file fail with an IO error (not "missing").
    // False if this medium can't simulate it here.
    virtual bool failReads(const std::string &slotFile, bool fail) = 0;
};

struct MemoryHarness : Harness {
    MemoryStorage medium;
    std::unique_ptr<SlotRecordStore> records = std::make_unique<SlotRecordStore>(medium, 1 << 16);
    RecordStore &store() override { return *records; }
    void reopen() override { records = std::make_unique<SlotRecordStore>(medium, 1 << 16); }
    bool failReads(const std::string &slotFile, bool fail) override {
        medium.failReads(slotFile, fail);
        return true;
    }
};

struct StdioHarness : Harness {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("hubbo_records_" + std::to_string(std::random_device{}()));
    std::unique_ptr<StdioStorage> medium;
    std::unique_ptr<SlotRecordStore> records;
    StdioHarness() {
        std::filesystem::create_directories(dir);
        reopen();
    }
    ~StdioHarness() override { std::filesystem::remove_all(dir); }
    RecordStore &store() override { return *records; }
    void reopen() override {
        records.reset();
        medium = std::make_unique<StdioStorage>(dir.string());
        records = std::make_unique<SlotRecordStore>(*medium, 1 << 16);
    }
    bool failReads(const std::string &slotFile, bool fail) override {
        if (::geteuid() == 0) return false;  // root reads through permissions
        std::filesystem::permissions(dir / slotFile,
                                     fail ? std::filesystem::perms::none
                                          : std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        return true;
    }
};

using Factory = std::function<std::unique_ptr<Harness>()>;

class RecordStoreContract : public ::testing::TestWithParam<Factory> {
protected:
    std::unique_ptr<Harness> h = GetParam()();
};

std::string loadPayload(RecordStore &store, const char *name, uint64_t *generation = nullptr) {
    Record r;
    if (store.load(name, &r) != ErrorCode::Ok) return "<none>";
    if (generation != nullptr) *generation = r.generation;
    return r.payload;
}

}  // namespace

TEST_P(RecordStoreContract, EmptyIsNotFound) {
    Record r;
    EXPECT_EQ(h->store().load("config", &r), ErrorCode::NotFound);
    EXPECT_EQ(h->store().health(), Health::Available);
}

TEST_P(RecordStoreContract, NewestGenerationWinsAcrossReopen) {
    RecordStore &s = h->store();
    ASSERT_EQ(s.save("config", 1, "one"), ErrorCode::Ok);
    ASSERT_EQ(s.save("config", 2, "two"), ErrorCode::Ok);
    ASSERT_EQ(s.save("config", 5, "five"), ErrorCode::Ok);
    h->reopen();
    uint64_t generation = 0;
    EXPECT_EQ(loadPayload(h->store(), "config", &generation), "five");
    EXPECT_EQ(generation, 5u);
}

TEST_P(RecordStoreContract, GenerationsOnlyGoUp) {
    RecordStore &s = h->store();
    ASSERT_EQ(s.save("config", 3, "three"), ErrorCode::Ok);
    EXPECT_EQ(s.save("config", 3, "again"), ErrorCode::Internal);
    EXPECT_EQ(s.save("config", 2, "older"), ErrorCode::Internal);
    EXPECT_EQ(loadPayload(s, "config"), "three");
}

TEST_P(RecordStoreContract, RecordsAreIndependentAndPayloadsExact) {
    RecordStore &s = h->store();
    std::string binary("\0\x01\xff{}", 5);
    ASSERT_EQ(s.save("config", 1, binary), ErrorCode::Ok);
    ASSERT_EQ(s.save("active_activity", 1, ""), ErrorCode::Ok);
    EXPECT_EQ(loadPayload(s, "config"), binary);
    EXPECT_EQ(loadPayload(s, "active_activity"), "");
    EXPECT_EQ(s.save("config", 2, std::string((1 << 16) + 1, 'x')), ErrorCode::TooLong);
}

// "Can't read" is never "doesn't exist": slot a holds generation 3 (the
// newest), slot b generation 2. With a unreadable, load must not return
// generation 2, and save must not overwrite a.
TEST_P(RecordStoreContract, AnUnreadableSlotFailsLoadAndSave) {
    RecordStore &s = h->store();
    ASSERT_EQ(s.save("config", 1, "one"), ErrorCode::Ok);    // a
    ASSERT_EQ(s.save("config", 2, "two"), ErrorCode::Ok);    // b
    ASSERT_EQ(s.save("config", 3, "three"), ErrorCode::Ok);  // a
    if (!h->failReads("config.a", true)) GTEST_SKIP() << "medium can't simulate read errors here";
    h->reopen();
    Record r;
    EXPECT_EQ(h->store().load("config", &r), ErrorCode::StorageFailed);
    EXPECT_EQ(h->store().save("config", 4, "four"), ErrorCode::StorageFailed);
    h->failReads("config.a", false);
    h->reopen();
    uint64_t generation = 0;
    EXPECT_EQ(loadPayload(h->store(), "config", &generation), "three");  // nothing was lost
    EXPECT_EQ(generation, 3u);
}

std::unique_ptr<Harness> memory() { return std::make_unique<MemoryHarness>(); }
std::unique_ptr<Harness> stdio() { return std::make_unique<StdioHarness>(); }

INSTANTIATE_TEST_SUITE_P(Implementations, RecordStoreContract, ::testing::Values(Factory(memory), Factory(stdio)));

// ---- crash injection (the memory medium can lose power mid-write) ---------

TEST(RecordStoreCrash, PowerLossAtEveryWriteKeepsTheLastAcknowledgedRecord) {
    const std::string next(300, 'n');
    for (int ops = 0; ops < 4; ops++) {
        for (size_t torn : {size_t{0}, size_t{1}, size_t{23}, size_t{24}, size_t{100}, next.size() + 23}) {
            for (int history = 1; history <= 3; history++) {  // 1..3 earlier saves (one or both slots full)
                MemoryHarness h;
                for (int g = 1; g <= history; g++) ASSERT_EQ(h.store().save("config", g, "gen" + std::to_string(g)), ErrorCode::Ok);
                h.medium.crashAfter(ops, torn);
                const ErrorCode saved = h.store().save("config", history + 1, next);
                h.medium.reboot();
                h.reopen();
                uint64_t generation = 0;
                const std::string loaded = loadPayload(h.store(), "config", &generation);
                if (saved == ErrorCode::Ok) {
                    EXPECT_EQ(loaded, next) << "acknowledged save lost; ops=" << ops << " torn=" << torn;
                } else {
                    EXPECT_EQ(loaded, "gen" + std::to_string(history))
                        << "after a failed save the old record must load; ops=" << ops << " torn=" << torn;
                    EXPECT_EQ(generation, static_cast<uint64_t>(history));
                }
            }
        }
    }
}

TEST(RecordStoreCrash, CorruptionInEitherSlotFallsBackToTheOther) {
    MemoryHarness h;
    ASSERT_EQ(h.store().save("config", 1, "first"), ErrorCode::Ok);   // slot a
    ASSERT_EQ(h.store().save("config", 2, "second"), ErrorCode::Ok);  // slot b
    std::string &b = h.medium.files()["config.b"];
    for (size_t i = 0; i < b.size(); i++) {
        std::string flipped = b;
        flipped[i] = static_cast<char>(flipped[i] ^ 0x5A);
        h.medium.files()["config.b"] = flipped;
        EXPECT_EQ(loadPayload(h.store(), "config"), "first") << "byte " << i;
    }
    h.medium.files()["config.b"] = std::string();  // truncated to nothing
    EXPECT_EQ(loadPayload(h.store(), "config"), "first");
}

TEST(RecordStoreCrash, AFailedVerifyInvalidatesTheSlot) {
    // A medium that silently drops writes: the read-back can't match.
    struct Lossy : MemoryStorage {
        bool lose = false;
        ErrorCode write(const std::string &name, std::string_view data) override {
            return MemoryStorage::write(name, lose ? data.substr(0, data.size() / 2) : data);
        }
    } medium;
    SlotRecordStore store(medium, 1024);
    ASSERT_EQ(store.save("config", 1, "good"), ErrorCode::Ok);
    medium.lose = true;
    EXPECT_EQ(store.save("config", 2, "lost"), ErrorCode::StorageFailed);
    medium.lose = false;
    Record r;
    ASSERT_EQ(store.load("config", &r), ErrorCode::Ok);
    EXPECT_EQ(r.payload, "good");
    EXPECT_EQ(r.generation, 1u);
}

TEST(RecordStoreHealth, UnavailableAndReadOnlyNeverFallBackToRam) {
    MemoryStorage medium;
    SlotRecordStore store(medium, 1024);
    ASSERT_EQ(store.save("config", 1, "kept"), ErrorCode::Ok);
    medium.setHealth(Health::ReadOnly);
    EXPECT_EQ(store.save("config", 2, "no"), ErrorCode::StorageFailed);
    Record r;
    EXPECT_EQ(store.load("config", &r), ErrorCode::Ok);  // reads still work
    medium.setHealth(Health::Unavailable);
    EXPECT_EQ(store.load("config", &r), ErrorCode::StorageFailed);
    EXPECT_EQ(store.health(), Health::Unavailable);
}

TEST(RecordStoreFraming, SlotsRoundTripAndRejectDamage) {
    const std::string bytes = encodeSlot(42, "payload");
    ASSERT_EQ(bytes.size(), kSlotHeaderBytes + 7);
    Record r;
    ASSERT_TRUE(decodeSlot(bytes, &r));
    EXPECT_EQ(r.generation, 42u);
    EXPECT_EQ(r.payload, "payload");
    EXPECT_FALSE(decodeSlot(bytes.substr(0, bytes.size() - 1), &r));
    EXPECT_FALSE(decodeSlot(std::string(kSlotHeaderBytes, '\0'), &r));  // invalidated slot
    EXPECT_FALSE(decodeSlot(encodeSlot(0, "x"), &r));                  // generation 0 never valid
    EXPECT_EQ(crc32(reinterpret_cast<const uint8_t *>("123456789"), 9), 0xCBF43926u);  // the IEEE check value
}

TEST(RecordStoreCrash, AnInvalidationThatFailsLeavesTheStoreReadOnly) {
    // The read-back doesn't match, and invalidating the slot fails too.
    struct Failing : MemoryStorage {
        bool lose = false;
        bool failInvalidate = false;
        ErrorCode write(const std::string &name, std::string_view data) override {
            const bool invalidation = data == std::string(kSlotHeaderBytes, '\0');
            if (invalidation && failInvalidate) return ErrorCode::StorageFailed;
            return MemoryStorage::write(name, lose ? data.substr(0, data.size() / 2) : data);
        }
    } medium;
    SlotRecordStore store(medium, 1024);
    ASSERT_EQ(store.save("config", 1, "good"), ErrorCode::Ok);
    medium.lose = true;
    medium.failInvalidate = true;
    EXPECT_EQ(store.save("config", 2, "lost"), ErrorCode::StorageFailed);
    EXPECT_EQ(store.health(), Health::ReadOnly);
    // The medium "recovers", but the store no longer trusts it: every later
    // save fails loudly, nothing is acknowledged.
    medium.lose = false;
    medium.failInvalidate = false;
    const int mutations = medium.mutations();
    EXPECT_EQ(store.save("config", 3, "after"), ErrorCode::StorageFailed);
    EXPECT_EQ(store.save("other", 1, "after"), ErrorCode::StorageFailed);
    EXPECT_EQ(medium.mutations(), mutations);
    EXPECT_EQ(store.health(), Health::ReadOnly);
    Record r;
    ASSERT_EQ(store.load("config", &r), ErrorCode::Ok);  // reads still work
    EXPECT_EQ(r.payload, "good");
}

TEST(RecordStoreGenerations, TheStoreOwnsThem) {
    MemoryStorage medium;
    SlotRecordStore store(medium, 1024);
    uint64_t next = 0;
    ASSERT_EQ(store.nextGeneration("config", &next), ErrorCode::Ok);
    EXPECT_EQ(next, 1u);
    ASSERT_EQ(store.save("config", 7, "seven"), ErrorCode::Ok);
    ASSERT_EQ(store.nextGeneration("config", &next), ErrorCode::Ok);
    EXPECT_EQ(next, 8u);
    // Another store over the same medium (another owner, or after a reboot)
    // sees the same next generation: nothing is cached by owners.
    SlotRecordStore other(medium, 1024);
    ASSERT_EQ(other.save("config", 8, "eight"), ErrorCode::Ok);
    ASSERT_EQ(store.nextGeneration("config", &next), ErrorCode::Ok);
    EXPECT_EQ(next, 9u);
    EXPECT_EQ(store.save("config", 8, "stale"), ErrorCode::Internal);
    medium.failReads("config.a");
    EXPECT_EQ(store.nextGeneration("config", &next), ErrorCode::StorageFailed);  // unreadable: unknown
}

TEST(RecordBacked, StateStore) {
    MemoryStorage medium;
    SlotRecordStore records(medium, 1024);
    RecordStateStore state(records);
    std::string v;
    EXPECT_EQ(state.get("active_activity", &v), ErrorCode::NotFound);
    ASSERT_TRUE(state.set("active_activity", "act_x"));
    ASSERT_TRUE(state.set("active_activity", ""));
    ASSERT_EQ(state.get("active_activity", &v), ErrorCode::Ok);
    EXPECT_EQ(v, "");
    // Generations come from the store: a second state store over the same
    // records continues where the first left off.
    RecordStateStore again(records);
    ASSERT_TRUE(again.set("active_activity", "act_y"));
    ASSERT_EQ(state.get("active_activity", &v), ErrorCode::Ok);
    EXPECT_EQ(v, "act_y");
    // Unreadable is unknown, never "absent"; and nothing is written over it.
    medium.failReads("active_activity.a");
    v = "unchanged";
    EXPECT_EQ(state.get("active_activity", &v), ErrorCode::StorageFailed);
    EXPECT_EQ(v, "unchanged");
    EXPECT_FALSE(state.set("active_activity", "act_z"));
}

// ---- absence needs proof (design part 6) -----------------------------------

TEST(RecordStoreAbsence, DamageWithNothingIntactIsCorruptNeverAbsent) {
    std::string newerFormat = encodeSlot(3, "from a newer firmware");
    newerFormat[4] = 2;  // slot format 2: unknown here
    const std::string damaged[] = {
        "garbage",                                  // not a slot
        encodeSlot(2, "x").substr(0, 10),           // torn header
        std::string(),                              // empty file (a torn first write)
        newerFormat,                                // unknown format
        std::string(kSlotHeaderBytes - 1, '\0'),    // a torn invalidation: not the marker
        encodeSlot(2, std::string(2000, 'x')),      // too large for this store
    };
    for (const std::string &bytes : damaged) {
        MemoryStorage medium;
        SlotRecordStore store(medium, 1024);
        medium.files()["config.a"] = bytes;
        Record r;
        EXPECT_EQ(store.load("config", &r), ErrorCode::Corrupt) << bytes.size();
        medium.files()["config.b"] = bytes;  // both slots damaged
        EXPECT_EQ(store.load("config", &r), ErrorCode::Corrupt) << bytes.size();
        medium.files()["config.b"] = invalidatedSlot();
        EXPECT_EQ(store.load("config", &r), ErrorCode::Corrupt) << bytes.size();
        // A save over it is possible (a restore/reset decides that), and
        // then the intact record wins.
        uint64_t next = 0;
        ASSERT_EQ(store.nextGeneration("config", &next), ErrorCode::Ok);
        ASSERT_EQ(store.save("config", next, "restored"), ErrorCode::Ok);
        ASSERT_EQ(store.load("config", &r), ErrorCode::Ok);
        EXPECT_EQ(r.payload, "restored");
    }
}

TEST(RecordStoreAbsence, OnlyMissingOrInvalidatedSlotsAreAbsent) {
    MemoryStorage medium;
    SlotRecordStore store(medium, 1024);
    Record r;
    EXPECT_EQ(store.load("config", &r), ErrorCode::NotFound);  // no slot file
    medium.files()["config.a"] = invalidatedSlot();
    EXPECT_EQ(store.load("config", &r), ErrorCode::NotFound);  // invalidated + absent
    medium.files()["config.b"] = invalidatedSlot();
    EXPECT_EQ(store.load("config", &r), ErrorCode::NotFound);  // both invalidated
    EXPECT_TRUE(isInvalidatedSlot(invalidatedSlot()));
    EXPECT_FALSE(decodeSlot(invalidatedSlot(), &r));
    // Every real slot starts with the magic, so no slot is ever the marker.
    EXPECT_FALSE(isInvalidatedSlot(encodeSlot(1, "").substr(0, kSlotHeaderBytes)));
}

TEST(RecordStoreAbsence, ATornSlotNextToAnIntactOneStillFallsBack) {
    MemoryStorage medium;
    SlotRecordStore store(medium, 1024);
    ASSERT_EQ(store.save("config", 1, "first"), ErrorCode::Ok);  // slot a
    medium.files()["config.b"] = encodeSlot(2, "second").substr(0, 27);  // torn payload
    Record r;
    ASSERT_EQ(store.load("config", &r), ErrorCode::Ok);
    EXPECT_EQ(r.payload, "first");
}

TEST(RecordStoreAbsence, PowerLossDuringTheFirstSaveIsCorrupt) {
    // The accepted cost (design part 6): nothing to fall back to.
    MemoryStorage medium;
    SlotRecordStore store(medium, 1024);
    medium.crashAfter(0, 10);
    EXPECT_EQ(store.save("config", 1, "first ever"), ErrorCode::StorageFailed);
    medium.reboot();
    SlotRecordStore reopened(medium, 1024);
    Record r;
    EXPECT_EQ(reopened.load("config", &r), ErrorCode::Corrupt);
}

// ---- one owner per medium (design part 6) ----------------------------------

TEST(RecordStoreTrust, EveryStoreOnTheMediumSeesADistrustedMedium) {
    struct Failing : MemoryStorage {
        bool fail = false;
        ErrorCode write(const std::string &name, std::string_view data) override {
            if (fail) return ErrorCode::StorageFailed;  // the save and its invalidation
            return MemoryStorage::write(name, data);
        }
    } medium;
    SlotRecordStore config(medium, 1024);
    SlotRecordStore secrets(medium, 64);  // another owner on the same medium
    ASSERT_EQ(secrets.save("secrets", 1, "s"), ErrorCode::Ok);
    medium.fail = true;
    EXPECT_EQ(config.save("config", 1, "c"), ErrorCode::StorageFailed);
    medium.fail = false;
    EXPECT_FALSE(medium.trusted());
    EXPECT_EQ(config.health(), Health::ReadOnly);
    EXPECT_EQ(secrets.health(), Health::ReadOnly);
    EXPECT_EQ(secrets.save("secrets", 2, "t"), ErrorCode::StorageFailed);
    SlotRecordStore later(medium, 1024);  // even one created afterwards
    EXPECT_EQ(later.health(), Health::ReadOnly);
    medium.reboot();  // a power cycle trusts it again
    EXPECT_EQ(secrets.save("secrets", 2, "t"), ErrorCode::Ok);
}
