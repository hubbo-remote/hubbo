#pragma once

// api::BlePairingPort over BleHid, the same paths as the console's `pair`
// and `ble forget`. httpd calls it; BleHid is loop-only, so status() answers
// from a copy the loop refreshes (publish()), and requests go through one
// mailbox slot the loop takes (poll()). Neither side waits for the other: a
// spinlock guards only the copies. Bond deletion still goes through BleHid's
// offload to httpd, never NVS on the RF task.
//
// publish() reads BleHid (state, pairingFailed()) and takes the peer address
// publishRuntime() already caches, since peerAddress() allocates.

#include <freertos/FreeRTOS.h>

#include "ble_hid.h"
#include "hubbo/api/ble_pairing_port.h"

namespace hubbo::fw {

class BlePairingRuntime : public hubbo::api::BlePairingPort {
public:
    explicit BlePairingRuntime(BleHid &ble) : ble_(ble) {}

    // httpd.
    hubbo::api::BlePairingStatus status() const override;
    hubbo::api::BlePairingResult startPairing(uint32_t windowMs) override;
    hubbo::api::BlePairingResult forget() override;

    // Loop: run a requested change; true if one ran (publish again at once).
    bool poll(uint32_t nowMs);
    // Loop, from publishRuntime(). peer is its cached peerAddress().
    void publish(uint32_t nowMs, const char *peer);

private:
    enum class Request : uint8_t { None, Start, Forget };
    struct Snapshot {  // no heap: copied under the spinlock
        hubbo::api::BlePairState state = hubbo::api::BlePairState::NotReady;
        uint32_t windowRemainingMs = 0;
        char peer[BleHid::kAddressSize] = {};
    };

    BleHid &ble_;
    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    Snapshot snapshot_;                  // guarded
    Request request_ = Request::None;    // guarded: the mailbox slot
    uint32_t requestWindowMs_ = 0;       // guarded
};

}  // namespace hubbo::fw
