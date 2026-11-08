// SnapshotStore: publish/read, keeping old snapshots alive for readers,
// bounded retirement, and a two-thread swap-during-read stress test.
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "hubbo/engine/snapshot.h"

using namespace hubbo;

namespace {

std::atomic<int> gLive{0};

struct Payload {
    explicit Payload(int v) : value(v), check(~v) { gLive++; }
    ~Payload() {
        check = 0;  // a use-after-free reader would see a broken invariant
        gLive--;
    }
    bool intact() const { return check == ~value; }
    int value;
    int check;
};

using Store = SnapshotStore<Payload>;

}  // namespace

TEST(Snapshot, EmptyStoreReadsNull) {
    Store store;
    Store::Reader reader;
    ASSERT_TRUE(store.attach(reader));
    auto g = reader.read();
    EXPECT_EQ(g.get(), nullptr);
    EXPECT_FALSE(g);
    EXPECT_EQ(g.serial(), 0u);
}

TEST(Snapshot, PublishThenRead) {
    {
        Store store;
        Store::Reader reader;
        store.attach(reader);
        ASSERT_EQ(store.publish(std::make_unique<Payload>(1)), ErrorCode::Ok);
        {
            auto g = reader.read();
            ASSERT_TRUE(g);
            EXPECT_EQ(g->value, 1);
            EXPECT_EQ(g.serial(), 1u);
        }
        ASSERT_EQ(store.publish(std::make_unique<Payload>(2)), ErrorCode::Ok);
        auto g = reader.read();
        EXPECT_EQ(g->value, 2);
        EXPECT_EQ(g.serial(), 2u);
        EXPECT_EQ(gLive.load(), 1);  // the first was freed: nobody held it
    }
    EXPECT_EQ(gLive.load(), 0);
}

TEST(Snapshot, OldSnapshotLivesUntilTheReaderLetsGo) {
    Store store;
    Store::Reader reader;
    store.attach(reader);
    store.publish(std::make_unique<Payload>(1));
    {
        auto g = reader.read();
        store.publish(std::make_unique<Payload>(2));
        EXPECT_EQ(g->value, 1);  // still the one we pinned
        EXPECT_TRUE(g->intact());
        EXPECT_EQ(store.retiredCount(), 1u);
        EXPECT_EQ(store.reclaim(), 1u);  // can't free it yet
    }
    EXPECT_EQ(store.reclaim(), 0u);
    EXPECT_EQ(gLive.load(), 1);
}

TEST(Snapshot, NestedGuardsPinTheSameSnapshot) {
    Store store;
    Store::Reader reader;
    store.attach(reader);
    store.publish(std::make_unique<Payload>(1));
    auto outer = reader.read();
    {
        auto inner = reader.read();
        store.publish(std::make_unique<Payload>(2));
        EXPECT_EQ(inner->value, 1);
    }
    store.reclaim();
    EXPECT_EQ(outer->value, 1);  // inner's release didn't unpin it
    EXPECT_TRUE(outer->intact());
}

TEST(Snapshot, PublishRefusesWhenTooManyOldSnapshotsArePinned) {
    Store store;
    Store::Reader readers[limits::kMaxSnapshotReaders];
    for (auto &r : readers) ASSERT_TRUE(store.attach(r));
    Store::Reader extra;
    EXPECT_FALSE(store.attach(extra));  // slots are bounded

    // Each reader pins a different generation.
    std::vector<Store::Guard> guards;
    store.publish(std::make_unique<Payload>(0));
    for (size_t i = 0; i < limits::kMaxRetiredSnapshots; i++) {
        guards.push_back(readers[i].read());
        ASSERT_EQ(store.publish(std::make_unique<Payload>(static_cast<int>(i + 1))), ErrorCode::Ok);
    }
    auto next = std::make_unique<Payload>(99);
    EXPECT_EQ(store.publish(next), ErrorCode::Busy);
    ASSERT_NE(next, nullptr);  // handed back on failure, so the caller can retry
    EXPECT_EQ(next->value, 99);
    guards.clear();
    EXPECT_EQ(store.publish(next), ErrorCode::Ok);
    EXPECT_EQ(next, nullptr);
}

TEST(Snapshot, DetachFreesTheSlot) {
    Store store;
    {
        Store::Reader r;
        for (size_t i = 0; i < limits::kMaxSnapshotReaders; i++) {
            Store::Reader tmp;
            ASSERT_TRUE(store.attach(tmp));
        }
        EXPECT_TRUE(store.attach(r));
    }
}

// One thread swaps configs as fast as it can while another keeps reading and
// checking that what it reads is intact (not freed underneath it).
TEST(Snapshot, ConcurrentSwapDuringReadsIsSafe) {
    {
        Store store;
        store.publish(std::make_unique<Payload>(0));
        std::atomic<bool> stop{false};
        std::atomic<long> reads{0};
        std::atomic<bool> corrupted{false};

        std::thread reader([&] {
            Store::Reader r;
            ASSERT_TRUE(store.attach(r));
            int last = 0;
            while (!stop.load()) {
                auto g = r.read();
                if (!g || !g->intact() || g->value < last) corrupted = true;
                last = g->value;
                for (int spin = 0; spin < 50; spin++) {
                    if (!g->intact()) corrupted = true;
                }
                reads++;
            }
        });

        int published = 1;
        for (; published < 20000; published++) {
            while (store.publish(std::make_unique<Payload>(published)) == ErrorCode::Busy) {
                std::this_thread::yield();
            }
        }
        while (reads.load() < 1000) std::this_thread::yield();
        stop = true;
        reader.join();
        EXPECT_FALSE(corrupted.load());
        store.reclaim();
        EXPECT_EQ(store.retiredCount(), 0u);
    }
    EXPECT_EQ(gLive.load(), 0);
}
