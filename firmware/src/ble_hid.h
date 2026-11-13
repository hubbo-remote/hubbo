#pragma once

// Bluetooth LE HID keyboard + consumer control (HID over GATT, NimBLE), for
// up to kMaxHosts hosts at once (docs/design/multi-bluetooth.md).
//
// The radio side of BLE HID only: links, pairing and sending reports. It
// holds no key state: it is the HidTransport behind core's BleHidAdapter,
// which decides which keys are down on which device.
//
// - Each host belongs to one hub device (its id). The table of device id ->
//   host identity address lives in NVS next to NimBLE's bonds, written only
//   when it changes (on httpd, through the offload).
// - There's no table of connection handles: a send asks the stack for the
//   live, encrypted connection to that device's identity address.
// - Every paired host stays connected. While one isn't, the hub advertises
//   with a connect whitelist of the paired identities. A pairing window
//   (one device at a time) turns the whitelist off; the first new bond in it
//   becomes that device's host, and only then is the device's old bond
//   deleted. The device's own host pairing again also ends it (nothing to
//   delete). A cancel or timeout keeps the old host.
// - NimBLE's bond-store-full handling (delete the oldest bond) is replaced
//   by a refusal, so a full store ends the window with no new host instead
//   of dropping a paired host.
//
// Ported from Harmonizer's src/bluetooth.cpp (soaresbrun0/Harmonizer,
// AGPL-3.0), including its advertising fix: the name goes in the scan
// response, or a name over ~10 characters doesn't fit and the device
// advertises with no name (the Apple TV then doesn't list it).
//
// All methods are for the main loop only (the engine runs there too).
// NimBLE callbacks run on the host task and only set flags that loop()
// picks up.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string_view>

#include "hubbo/domain/ids.h"
#include "hubbo/integrations/ble_hid.h"
#include "hubbo/integrations/ble_hosts.h"

class NimBLECharacteristic;

namespace hubbo::fw {

class BleHid : public hubbo::integrations::HidTransport {
public:
    static constexpr size_t kMaxHosts = hubbo::integrations::kMaxBleDevices;

    bool begin(const char *name);
    bool started() const { return started_; }
    // The hub's name (hub_name.h), live: GAP Device Name and scan response,
    // advertising restarted. Paired hosts stay paired.
    void setName(std::string_view name);
    void loop(uint32_t nowMs);

    // Bond deletions and the host table write NVS, which the RF task must
    // never do: main hands those jobs to httpd. Returns false if it couldn't
    // queue the job. Unset (boot): done inline.
    using Offload = std::function<bool(std::function<void()>)>;
    void setOffload(Offload offload) { offload_ = std::move(offload); }

    // HidTransport. connected(): that device's host is linked and encrypted
    // (the state loop() refreshes on every link event). The sends ask the
    // stack for that host's live connection and return false without one, if
    // a usage is outside the report map, or if the notify failed; they queue
    // to the NimBLE host task.
    bool connected(std::string_view deviceId) const override;
    bool sendKeyboard(std::string_view deviceId, const uint8_t report[8]) override;
    bool sendConsumer(std::string_view deviceId, uint16_t usage) override;

    // Pairing, per device. startPairing() is false if the radio isn't up, a
    // window is already open, or the id isn't one. cancelPairing() closes
    // only this device's.
    bool startPairing(std::string_view deviceId, uint32_t nowMs, uint32_t windowMs);
    void cancelPairing(std::string_view deviceId);
    // Disconnects and deletes this device's bond. Remove the device on the
    // host too (Apple TV: Settings > Remotes and Devices > Bluetooth).
    void forget(std::string_view deviceId);
    // Forgets every host, and closes the window, whose device isn't in
    // `devices` (the saved config's).
    void retainOnly(const hubbo::integrations::BleDeviceIds &devices);

    hubbo::integrations::BleHostStatus status(std::string_view deviceId, uint32_t nowMs) const;
    // Every device with a status other than "not paired": the paired ones
    // and the window's. Plain data, for BleHostRuntime.
    struct Snapshot {
        struct Entry {
            hubbo::IdString deviceId;
            hubbo::integrations::BleHostStatus status;
        };
        bool radioReady = false;
        Entry entries[kMaxHosts + 1];
        size_t count = 0;
    };
    void snapshot(Snapshot &out, uint32_t nowMs) const;
    // Console `status`: the radio, then one line per paired host and the
    // window. Allocates.
    void printStatus(uint32_t nowMs) const;

private:
    struct Host {
        hubbo::IdString deviceId;  // empty = free slot
        uint8_t addrType = 0;      // identity address (ble_addr_t)
        uint8_t addr[6] = {};
        bool linked = false;       // loop's view, refreshed on link events
        bool paired() const { return !deviceId.empty(); }
    };

    bool windowOpen() const { return !windowDevice_.empty(); }
    Host *find(std::string_view deviceId);
    const Host *find(std::string_view deviceId) const;
    Host *findByIdentity(uint8_t type, const uint8_t addr[6]);
    bool connHandleOf(const Host &host, uint16_t *handle = nullptr) const;
    void disconnect(const Host &host);
    void forgetHost(Host &host);  // no table save or advertising update
    bool notify(std::string_view deviceId, NimBLECharacteristic *input, const uint8_t *report, size_t length);
    void logLinkEvents();
    void refreshLinks();
    void adoptBond(uint16_t handle, uint8_t type, const uint8_t addr[6]);
    void closeWindow();
    void deleteBond(uint8_t type, const uint8_t addr[6]);
    void saveHosts();
    void loadHosts();
    void runNvsJob(std::function<void()> job);
    void updateAdvertising();
    void setScanName(std::string_view name);

    bool started_ = false;
    Host hosts_[kMaxHosts];
    // The pairing window: open while windowDevice_ is set.
    hubbo::IdString windowDevice_;
    uint32_t windowStartedMs_ = 0;
    uint32_t windowMs_ = 0;
    // Advertising: the whitelist matches hosts_, and the filter in effect.
    bool whitelistDirty_ = true;
    bool advertisingFiltered_ = false;
    uint32_t advertisingCheckedMs_ = 0;
    Offload offload_;
    std::deque<std::function<void()>> pendingJobs_;  // waiting for room in the offload queue, in order
    uint32_t notifyFailures_ = 0;
};

}  // namespace hubbo::fw
