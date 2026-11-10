#pragma once

// api::BlePairingPort over BleHid, the same paths as the console's `pair`
// and `ble forget`. httpd calls it; BleHid is loop-only, so status() answers
// from a copy the loop refreshes (publish()), and requests go through one
// mailbox slot the loop takes (poll()). Neither side waits for the other: a
// spinlock guards only the copies. Bond deletion still goes through BleHid's
// offload to httpd, never NVS on the RF task.
//
// publish() reads BleHid (state, pairingFailed(), and the peer address only
// when the state changes, since peerAddress() allocates). It maps
// BleHid::State onto the API's terms once; link() hands the same copy to
// EngineRuntime for core's shared ble_hid state functions (ble_state.h).

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

    // Any task: the latest publish(), without pending requests applied.
    struct Link {
        hubbo::api::BlePairState pairing = hubbo::api::BlePairState::NotReady;
        bool hostConnected = false;  // Paired only
        uint32_t windowRemainingMs = 0;  // Pairing only
        char peer[BleHid::kAddressSize] = {};  // Paired only
    };
    Link link() const;

    // Loop: run a requested change; true if one ran (publish again at once).
    bool poll(uint32_t nowMs);
    // Loop, from publishRuntime().
    void publish(uint32_t nowMs);

private:
    enum class Request : uint8_t { None, Start, Forget };
    BleHid &ble_;
    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    Link snapshot_;                      // guarded; no heap
    Request request_ = Request::None;    // guarded: the mailbox slot
    uint32_t requestWindowMs_ = 0;       // guarded
    // Loop only: the peer address, read when the state last changed.
    BleHid::State peerState_ = BleHid::State::Off;
    bool peerRead_ = false;
    char peer_[BleHid::kAddressSize] = {};
};

}  // namespace hubbo::fw
