#pragma once

// integrations::BleHostPort over BleHid: what the admin API drives (pair,
// cancel, forget, status) and what EngineRuntime reads for the `ble`
// integration and each Bluetooth device.
//
// httpd calls the port; BleHid is loop-only. So status() answers from a
// snapshot the loop publishes, and requests go into a small queue the loop
// drains (poll()). The config's device set (retainOnly(), from the config
// task) is one latest-wins slot, applied after the queue once the radio is
// up. Neither side waits for the other: a spinlock guards only the copies
// (no heap inside it). status() and startPairing()'s Busy check see pending
// requests already applied, so a pair right after a cancel works.
// Bond deletion and the host table still go through BleHid's offload to
// httpd, never NVS on the RF task.

#include <freertos/FreeRTOS.h>

#include "ble_hid.h"
#include "hubbo/integrations/ble_hosts.h"

namespace hubbo::fw {

class BleHostRuntime : public hubbo::integrations::BleHostPort {
public:
    explicit BleHostRuntime(BleHid &ble) : ble_(ble) {}

    // httpd (any task).
    bool radioReady() const override;
    hubbo::integrations::BleHostStatus status(std::string_view deviceId) const override;
    hubbo::integrations::BleHostResult startPairing(std::string_view deviceId, uint32_t windowMs) override;
    hubbo::integrations::BleHostResult cancelPairing(std::string_view deviceId) override;
    hubbo::integrations::BleHostResult forget(std::string_view deviceId) override;
    // The config task.
    void retainOnly(const hubbo::integrations::BleDeviceIds &devices) override;

    // Loop: run queued requests, then publish at once.
    void poll(uint32_t nowMs);
    // Loop, from publishRuntime().
    void publish(uint32_t nowMs);

private:
    enum class Kind : uint8_t { Start, Cancel, Forget };
    struct Request {
        Kind kind = Kind::Start;
        hubbo::IdString deviceId;
        uint32_t windowMs = 0;
    };
    static constexpr size_t kMaxRequests = 8;

    hubbo::integrations::BleHostResult enqueue(Kind kind, std::string_view deviceId, uint32_t windowMs);
    // Under the lock: a device's status with the queued requests applied.
    hubbo::integrations::BleHostStatus statusLocked(std::string_view deviceId) const;
    struct Window {
        hubbo::IdString device;  // empty: none
        uint32_t remainingMs = 0;
    };
    // Under the lock: the window once the pending requests have run.
    Window windowLocked() const;
    // Loop: BleHid's state now, for httpd; then drop the first `ran`
    // requests, and the pending set if `retained` and no newer one came in.
    void store(uint32_t nowMs, size_t ran, bool retained, uint32_t retainSeq);

    BleHid &ble_;
    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    BleHid::Snapshot snapshot_;        // guarded
    Request requests_[kMaxRequests];   // guarded, in order
    size_t requestCount_ = 0;          // guarded
    hubbo::integrations::BleDeviceIds retain_;  // guarded: the latest set
    bool retainPending_ = false;                // guarded
    uint32_t retainSeq_ = 0;                    // guarded: bumped by each retainOnly()
};

}  // namespace hubbo::fw
