#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hubbo/config/service.h"
#include "hubbo/domain/config.h"
#include "hubbo/domain/ids.h"
#include "hubbo/domain/limits.h"

// Several Bluetooth hosts (docs/design/multi-bluetooth.md). The `ble`
// integration is the radio only; each host is a device under it, with its
// own bond, pairing window and connection. There's no "current host": a key
// goes to the host of the device its action names.
//
// BleHostPort is what the admin API drives (pair, cancel, forget, status)
// and what the platform implements: the firmware over NimBLE, hub-sim with
// SimulatedBleHosts. Like RemotePort, every method is called on the HTTP
// task and returns quickly: requests are handed to the BLE loop, and
// status() answers from a copy the loop keeps current.
namespace hubbo::integrations {

constexpr size_t kMaxBleDevices = limits::kMaxBleDevices;  // bonds and connections at once

constexpr uint32_t kBlePairingDefaultWindowMs = 120000;
constexpr uint32_t kBlePairingMinWindowMs = 30000;
constexpr uint32_t kBlePairingMaxWindowMs = 300000;

// One Bluetooth device's bond, pairing window and link.
struct BleHostStatus {
    bool bonded = false;             // it has a host
    bool connected = false;          // bonded, and that host's link is up
    bool pairing = false;            // its pairing window is open (the old bond, if any, still works)
    uint32_t windowRemainingMs = 0;  // pairing only
};

// The Bluetooth devices a config has: its ble_hid devices' ids.
struct BleDeviceIds {
    IdString ids[kMaxBleDevices];
    size_t count = 0;
    bool contains(std::string_view deviceId) const;
};
// False (and `out` incomplete) if the config has more than kMaxBleDevices,
// which no compiled config does.
bool bleDeviceIds(const ConfigSpec &config, BleDeviceIds *out);

enum class BleHostResult : uint8_t {
    Accepted,  // handed to the BLE loop (or nothing to do); status() shows the effect shortly
    NotReady,  // the radio isn't working
    Busy,      // startPairing() only: another device's window is open
    Full,      // the BLE loop's request queue is full: try again
};

class BleHostPort {
public:
    virtual ~BleHostPort() = default;

    // The radio is up (the integration's health).
    virtual bool radioReady() const = 0;
    // A device's bond and link. An unknown id: all false (not paired).
    virtual BleHostStatus status(std::string_view deviceId) const = 0;
    // Open a pairing window for `deviceId` (the route bounds `windowMs`).
    // The first new bond in the window becomes its host, and only then is
    // its old bond (if any) deleted; the host it already had bonding again
    // counts too. A cancel, a timeout or a full bond store closes the window
    // and keeps what was there (the old host, or not paired). Accepted, and
    // nothing changes, when this device's window is already open; Busy when
    // another device's is.
    virtual BleHostResult startPairing(std::string_view deviceId, uint32_t windowMs) = 0;
    // Close the window if it's this device's (Accepted when there's none).
    // Never Busy.
    virtual BleHostResult cancelPairing(std::string_view deviceId) = 0;
    // Delete this device's bond, disconnecting its host. Accepted when
    // there's nothing to forget. Never Busy.
    virtual BleHostResult forget(std::string_view deviceId) = 0;
    // Reconcile with the saved config: forget every host (and close any
    // window) whose device id isn't in `devices`. Never refused: the latest
    // set wins, and the BLE loop applies it when it can (when the radio is
    // up). BleHostReconciler calls it for every config the hub runs.
    virtual void retainOnly(const BleDeviceIds &devices) = 0;
};

// Keeps a BleHostPort's hosts in step with the config: at boot once a saved
// config runs, and after every commit (create, delete, restore, reset).
// Nothing while Blocked or unsaved: absence needs proof.
class BleHostReconciler : public config::ConfigListener {
public:
    explicit BleHostReconciler(BleHostPort &port) : port_(port) {}
    void configSaved(const ConfigSpec &config) override;

private:
    BleHostPort &port_;
};

// What the API shows. The firmware and hub-sim share these, so the board
// and the sim always say the same thing.

// A Bluetooth device's state, as the API shows it: "pairing" while its
// window is open (windowS: whole seconds left, rounded up; else 0), then
// "connected" or "not_connected" when bonded, else "not_paired".
struct BleDeviceState {
    const char *state;
    uint32_t windowS;
};
BleDeviceState bleDeviceState(const BleHostStatus &status);
// A Bluetooth device's availability: "available" while its host is
// connected (a key would be sent), else "unavailable".
const char *bleDeviceAvailability(const BleHostStatus &status);
// The `ble` integration (the radio): "disabled" first, then "connected"
// when the radio is up, else "error".
const char *bleRadioState(const IntegrationSpec &integration, bool radioReady);
// Its detail line: "" when up, else "Bluetooth isn't working".
std::string bleRadioDetail(bool radioReady);

}  // namespace hubbo::integrations
