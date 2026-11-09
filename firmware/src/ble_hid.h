#pragma once

// Bluetooth LE HID keyboard + consumer control (HID over GATT, NimBLE).
//
// The radio side of BLE HID only: link, pairing and sending reports. It holds
// no key state: it is the HidTransport behind the core's BleHidAdapter, which
// decides which keys are down.
//
// - One bonded host. Just Works bonding, allowed only while the pairing
//   window is open; pairing a new host replaces the old one.
//
// Ported from Harmonizer's src/bluetooth.cpp (soaresbrun0/Harmonizer,
// AGPL-3.0), including its advertising fix: the scan response is enabled
// before setName(), or a name over ~10 characters doesn't fit and the
// device advertises with no name (the Apple TV then doesn't list it).
//
// All methods are for the main loop only. NimBLE callbacks run on the host
// task and only set flags that loop() picks up.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "hubbo/integrations/ble_hid.h"

namespace hubbo::fw {

class BleHid : public hubbo::integrations::HidTransport {
public:
    enum class State : uint8_t {
        Off,        // not started (or failed)
        NotPaired,  // no bonded host, not advertising
        Pairing,    // pairing window open, discoverable
        Waiting,    // bonded host known, advertising for it to reconnect
        Connected,  // bonded host connected and encrypted
    };

    static constexpr uint32_t kDefaultPairingWindowMs = 120000;

    bool begin(const char *name);
    void loop(uint32_t nowMs);

    // Deleting bonds writes NVS (NimBLE's bond store), which the RF task must
    // never do: main hands those jobs to httpd. Returns false if it couldn't
    // queue the job. Unset: done inline.
    using Offload = std::function<bool(std::function<void()>)>;
    void setOffload(Offload offload) { offload_ = std::move(offload); }

    // HidTransport. connected(): bonded host connected and encrypted. The
    // sends return false if not connected or the notify failed; they queue
    // to the NimBLE host task and don't block.
    bool connected() const override;
    bool sendKeyboard(const uint8_t report[8]) override;
    bool sendConsumer(uint16_t usage) override;
    // Sends empty reports (nothing held), e.g. before a reboot.
    void releaseAll();

    // Opens the pairing window. Forgets the current host first: only one
    // host is kept, and a connected one would take the only slot.
    bool startPairing(uint32_t nowMs, uint32_t windowMs = kDefaultPairingWindowMs);
    void stopPairing();
    uint32_t pairingSecondsLeft(uint32_t nowMs) const;

    // Disconnects and deletes every bond. Remove the device on the host too
    // (Apple TV: Settings > Remotes and Devices > Bluetooth).
    void forget();

    State state() const;
    static const char *stateName(State state);
    int bondCount() const { return bondCount_; }
    // Connected host's address, else the bonded one's; empty if none.
    void peerAddress(char *out, size_t size) const;

    uint32_t notifyFailures() const { return notifyFailures_; }

private:
    void updateAdvertising();
    void runBondJob(std::function<void()> job);

    bool started_ = false;
    bool pairing_ = false;
    bool bondingOpen_ = false;              // pairing_ and the old bonds are gone
    std::atomic<bool> bondsClearing_{false};  // a bond deletion is queued or running
    Offload offload_;
    std::function<void()> pendingBondJob_;  // waiting for room in the offload queue
    uint32_t pairingStartedMs_ = 0;
    uint32_t pairingWindowMs_ = 0;
    int bondCount_ = 0;
    uint32_t notifyFailures_ = 0;
};

}  // namespace hubbo::fw
