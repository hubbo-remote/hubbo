#include "latency.h"

#include <algorithm>

namespace hubbo::fw {

void LatencyStats::record(uint32_t us) {
    samples_[next_] = us;
    next_ = (next_ + 1) % kCapacity;
    count_++;
    if (us > max_) {
        max_ = us;
    }
}

LatencyStats::Summary LatencyStats::summarize() const {
    static uint32_t sorted[kCapacity];  // console only; keeps 2 KB off the stack
    Summary summary;
    summary.count = count_;
    summary.max = max_;
    const size_t kept = count_ < kCapacity ? count_ : kCapacity;
    summary.kept = static_cast<uint32_t>(kept);
    if (kept == 0) {
        return summary;
    }
    std::copy(samples_, samples_ + kept, sorted);
    std::sort(sorted, sorted + kept);
    // Nearest-rank percentile.
    auto rank = [&](unsigned percent) {
        size_t index = (kept * percent + 99) / 100;
        return sorted[index == 0 ? 0 : index - 1];
    };
    summary.p50 = rank(50);
    summary.p95 = rank(95);
    summary.p99 = rank(99);
    return summary;
}

void LatencyStats::reset() {
    next_ = 0;
    count_ = 0;
    max_ = 0;
}

}  // namespace hubbo::fw
