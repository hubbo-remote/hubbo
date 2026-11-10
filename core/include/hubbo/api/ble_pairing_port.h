#pragma once

#include <cstdint>
#include <string>

// BLE HID pairing as the admin API sees it (docs/design/phase4-web-admin.md,
// section 3a): POST/GET/DELETE /admin/integrations/{id}/pair for the ble_hid
// integration.
//
// The firmware implements it over BleHid (the console's `pair` and `ble
// forget`); hub-sim uses SimulatedBlePairing below. Like RemotePort, every
// method is called on the HTTP task and must return quickly: never block on,
// or run on, the RF loop. An implementation hands requests to the loop and
// answers status() from a copy the loop keeps up to date. Deleting bonds
// writes NVS, which stays off the RF task as it is today.
namespace hubbo::api {

constexpr uint32_t kBlePairingDefaultWindowMs = 120000;  // BleHid::kDefaultPairingWindowMs
constexpr uint32_t kBlePairingMinWindowMs = 30000;
constexpr uint32_t kBlePairingMaxWindowMs = 300000;

enum class BlePairState : uint8_t {
    NotReady,  // the BLE stack isn't running
    Idle,      // no bonded host, no window open
    Pairing,   // window open, discoverable; the old bond is already gone
    Paired,    // a bonded host (connected or not)
    Failed,    // the latest window closed with no new bond (until the next start or forget)
};
// PairingStatus.state: "idle", "pairing", "paired", "failed" (NotReady is a 503, never shown).
const char *blePairStateName(BlePairState state);

struct BlePairingStatus {
    BlePairState state = BlePairState::NotReady;
    uint32_t windowRemainingMs = 0;  // Pairing only
    std::string peerName;            // Paired: the host's name or address ("" if unknown), <= 48 chars
};

enum class BlePairingResult : uint8_t {
    Accepted,  // handed to the BLE loop; status() shows the effect shortly
    NotReady,  // the BLE stack isn't running
    Busy,      // startPairing(): a window is already open
};

class BlePairingPort {
public:
    virtual ~BlePairingPort() = default;

    virtual BlePairingStatus status() const = 0;
    // Open a pairing window of `windowMs` (the route bounds it to
    // kBlePairingMinWindowMs..kBlePairingMaxWindowMs). Forgets the current
    // bond first: a new bond replaces the old one. Busy while a window is open.
    virtual BlePairingResult startPairing(uint32_t windowMs) = 0;
    // Close any open window and delete the bond (disconnecting the host).
    // Accepted when there's nothing to forget.
    virtual BlePairingResult forget() = 0;
};

// In-memory BLE for hub-sim and tests: a window closes as Failed when
// advance() passes it, or Paired when bonded() is called. Single task.
class SimulatedBlePairing : public BlePairingPort {
public:
    SimulatedBlePairing() { status_.state = BlePairState::Idle; }
    BlePairingStatus status() const override;
    BlePairingResult startPairing(uint32_t windowMs) override;
    BlePairingResult forget() override;

    // Simulation controls.
    void advance(uint32_t elapsedMs);          // window timeout
    void bonded(const std::string &peerName);  // a host paired during the window
    bool ready = true;                         // false: NotReady

private:
    BlePairingStatus status_;
};

}  // namespace hubbo::api
