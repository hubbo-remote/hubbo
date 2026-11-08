#pragma once

#include <atomic>
#include <cstddef>

// Bounded single-producer/single-consumer ring. Lock-free, no allocation.
// One task pushes, one task pops. Holds N-1 items.
namespace hubbo {

template <typename T, size_t N>
class SpscRing {
    static_assert(N >= 2, "ring needs at least 2 slots");

public:
    // Producer side. Returns false when full (the item is not queued).
    bool push(const T &item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % N;
        if (next == tail_.load(std::memory_order_acquire)) return false;
        items_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false when empty.
    bool pop(T *out) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        *out = items_[tail];
        tail_.store((tail + 1) % N, std::memory_order_release);
        return true;
    }

private:
    T items_[N];
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

}  // namespace hubbo
