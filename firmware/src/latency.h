#pragma once

// Fixed-size latency recorder for the Phase 0.3 baseline. record() is O(1)
// and allocation-free (hot path safe); summarize() sorts a copy and is for
// the serial console only.

#include <cstddef>
#include <cstdint>

namespace hubbo::fw {

class LatencyStats {
public:
    static constexpr size_t kCapacity = 512;  // most recent samples kept

    struct Summary {
        uint32_t count = 0;  // all samples since reset (not just those kept)
        uint32_t kept = 0;
        uint32_t p50 = 0;
        uint32_t p95 = 0;
        uint32_t p99 = 0;
        uint32_t max = 0;  // over all samples since reset
    };

    void record(uint32_t us);
    Summary summarize() const;
    void reset();

private:
    uint32_t samples_[kCapacity] = {};
    size_t next_ = 0;
    uint32_t count_ = 0;
    uint32_t max_ = 0;
};

}  // namespace hubbo::fw
