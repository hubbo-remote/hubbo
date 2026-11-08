#pragma once

// Bluetooth LE HID keyboard + consumer control (HID over GATT, NimBLE).
//
// The radio side of BLE HID only: link, pairing and sending reports. It holds
// no key state. Which keys are down is the core's BleHidAdapter's job; this
// class is shaped to back its HidTransport (connected / sendKeyboard /
// sendConsumer) directly.
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

#include <cstddef>
#include <cstdint>

namespace hubbo::fw {

class BleHid {
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

    // Bonded host connected and encrypted: reports can be sent.
    bool connected() const;
    // Keyboard input report [modifiers, reserved, k1..k6]. False if not
    // connected or the notify failed. Doesn't block.
    bool sendKeyboard(const uint8_t report[8]);
    // Consumer control usage, 0 = released.
    bool sendConsumer(uint16_t usage);
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

    bool started_ = false;
    bool pairing_ = false;
    uint32_t pairingStartedMs_ = 0;
    uint32_t pairingWindowMs_ = 0;
    int bondCount_ = 0;
    uint32_t notifyFailures_ = 0;
};

}  // namespace hubbo::fw
