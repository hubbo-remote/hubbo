#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "hubbo/domain/error.h"
#include "hubbo/domain/limits.h"

// Publish/read of immutable snapshots (the compiled config) across tasks,
// without locks and without depending on FreeRTOS.
//
// One writer (the config task) publishes new snapshots. Up to
// kMaxSnapshotReaders readers (the RF/engine task, and optionally others)
// each own a Reader and take short-lived Guards around their use.
//
// Mechanism: hazard pointers. A reader announces the snapshot it is about to
// use in its own slot, then re-checks that it is still current. The writer
// swaps the current pointer, puts the old one on a bounded retired list, and
// frees a retired snapshot only once no reader slot names it. So:
//   * readers never block, never allocate and never free (hot-path safe);
//   * an old snapshot stays alive until every reader has dropped it;
//   * freeing happens on the writer side only (publish()/reclaim()).
//
// Overflow: if kMaxRetiredSnapshots old snapshots are all still in use,
// publish() refuses with ErrorCode::Busy and changes nothing; the caller
// retries later. With guards held only for one dispatch this should not
// happen in practice.
//
// Every publish gets a new serial (starting at 1), so a reader can detect
// "config changed" without comparing pointers (which could be reused).
//
// Requires lock-free pointer atomics (true on ESP32-S3 and hosts).
namespace hubbo {

template <typename T>
class SnapshotStore {
    struct Holder {
        std::unique_ptr<T> value;
        uint32_t serial;
    };

public:
    class Reader;

    // A pinned snapshot. get() is null if nothing was ever published.
    class Guard {
    public:
        Guard(Guard &&other) noexcept : reader_(other.reader_), holder_(other.holder_) {
            other.reader_ = nullptr;
            other.holder_ = nullptr;
        }
        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;
        Guard &operator=(Guard &&) = delete;
        ~Guard() {
            if (reader_ != nullptr) reader_->release();
        }

        const T *get() const { return holder_ != nullptr ? holder_->value.get() : nullptr; }
        const T *operator->() const { return get(); }
        explicit operator bool() const { return get() != nullptr; }
        uint32_t serial() const { return holder_ != nullptr ? holder_->serial : 0; }

    private:
        friend class Reader;
        Guard(Reader *reader, Holder *holder) : reader_(reader), holder_(holder) {}
        Reader *reader_;
        Holder *holder_;
    };

    // One per reading task. Not itself thread-safe: a Reader belongs to one
    // task. Nested guards on the same Reader are allowed and pin the same
    // snapshot as the outermost one.
    class Reader {
    public:
        Reader() = default;
        Reader(const Reader &) = delete;
        Reader &operator=(const Reader &) = delete;
        ~Reader() { detach(); }

        bool attached() const { return store_ != nullptr; }

        Guard read() {
            if (store_ == nullptr) return Guard(nullptr, nullptr);
            if (depth_ == 0) {
                auto &slot = store_->hazards_[slot_];
                Holder *p = store_->current_.load(std::memory_order_acquire);
                for (;;) {
                    slot.store(p, std::memory_order_seq_cst);
                    Holder *q = store_->current_.load(std::memory_order_seq_cst);
                    if (q == p) break;
                    p = q;
                }
                pinned_ = p;
            }
            depth_++;
            return Guard(this, pinned_);
        }

    private:
        friend class SnapshotStore;
        void release() {
            if (--depth_ == 0) {
                store_->hazards_[slot_].store(nullptr, std::memory_order_release);
                pinned_ = nullptr;
            }
        }
        void detach() {
            if (store_ != nullptr) {
                store_->hazards_[slot_].store(nullptr, std::memory_order_release);
                store_->slotUsed_[slot_].store(false, std::memory_order_release);
                store_ = nullptr;
            }
        }

        SnapshotStore *store_ = nullptr;
        size_t slot_ = 0;
        uint32_t depth_ = 0;
        Holder *pinned_ = nullptr;
    };

    SnapshotStore() {
        for (auto &h : hazards_) h.store(nullptr);
        for (auto &u : slotUsed_) u.store(false);
    }
    SnapshotStore(const SnapshotStore &) = delete;
    SnapshotStore &operator=(const SnapshotStore &) = delete;

    // Readers must be detached (destroyed) first.
    ~SnapshotStore() {
        delete current_.load();
        for (size_t i = 0; i < retiredCount_; i++) delete retired_[i];
    }

    // Claims a reader slot. Returns false when all slots are taken.
    bool attach(Reader &reader) {
        if (reader.store_ != nullptr) return false;
        for (size_t i = 0; i < limits::kMaxSnapshotReaders; i++) {
            bool expected = false;
            if (slotUsed_[i].compare_exchange_strong(expected, true)) {
                reader.store_ = this;
                reader.slot_ = i;
                return true;
            }
        }
        return false;
    }

    // Writer side; one writer at a time (a second concurrent publish gets
    // Busy). Takes ownership of `next` on success only, so the caller can
    // retry after Busy.
    ErrorCode publish(std::unique_ptr<T> &next) {
        if (!next) return ErrorCode::Internal;
        if (writing_.test_and_set(std::memory_order_acquire)) return ErrorCode::Busy;
        reclaimLocked();
        if (retiredCount_ == limits::kMaxRetiredSnapshots) {
            writing_.clear(std::memory_order_release);
            return ErrorCode::Busy;
        }
        auto *holder = new Holder{std::move(next), ++serial_};
        Holder *old = current_.exchange(holder, std::memory_order_seq_cst);
        if (old != nullptr) retired_[retiredCount_++] = old;
        reclaimLocked();
        writing_.clear(std::memory_order_release);
        return ErrorCode::Ok;
    }

    // Convenience for temporaries: on failure the snapshot is destroyed.
    ErrorCode publish(std::unique_ptr<T> &&next) {
        std::unique_ptr<T> owned(std::move(next));
        return publish(owned);
    }

    // Writer side: frees retired snapshots no reader still uses. Returns how
    // many remain retired (still pinned by some reader).
    size_t reclaim() {
        if (writing_.test_and_set(std::memory_order_acquire)) return retiredCount_;
        reclaimLocked();
        const size_t remaining = retiredCount_;
        writing_.clear(std::memory_order_release);
        return remaining;
    }

    size_t retiredCount() const { return retiredCount_; }
    uint32_t serial() const { return serial_; }

private:
    void reclaimLocked() {
        size_t kept = 0;
        for (size_t i = 0; i < retiredCount_; i++) {
            Holder *r = retired_[i];
            bool pinned = false;
            for (const auto &h : hazards_) {
                if (h.load(std::memory_order_seq_cst) == r) {
                    pinned = true;
                    break;
                }
            }
            if (pinned) {
                retired_[kept++] = r;
            } else {
                delete r;
            }
        }
        retiredCount_ = kept;
    }

    std::atomic<Holder *> current_{nullptr};
    std::atomic<Holder *> hazards_[limits::kMaxSnapshotReaders];
    std::atomic<bool> slotUsed_[limits::kMaxSnapshotReaders];
    std::atomic_flag writing_ = ATOMIC_FLAG_INIT;
    Holder *retired_[limits::kMaxRetiredSnapshots] = {};
    size_t retiredCount_ = 0;
    uint32_t serial_ = 0;
};

}  // namespace hubbo
