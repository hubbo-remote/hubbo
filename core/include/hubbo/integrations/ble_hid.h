#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "hubbo/domain/adapter.h"
#include "hubbo/domain/ids.h"
#include "hubbo/domain/limits.h"
#include "hubbo/domain/params.h"

// Generic BLE HID keyboard integration (Apple TV paired as a keyboard).
//
// Commands (same for every device of this integration):
//   key_down {key}   press and hold
//   key_up   {key}   release
//   key_tap  {key}   press + release
// `key` is an enum of HID keys (bleHidKeys()). The usages are the ones the
// Harmonizer spike verified on tvOS (Harmony's "BT Apple TV 5" profile and
// pyatv), not guesses.
//
// Each Bluetooth device is its own host (docs/design/multi-bluetooth.md):
// a key goes to the host of the device its action names, never elsewhere.
// BleHidAdapter holds the report state per device (6-key keyboard rollover
// plus one consumer usage) and talks to the radio through HidTransport,
// which the firmware implements with NimBLE and the simulator records. Held
// keys are released on releaseAll() (activity switch, lost release) and a
// device's are dropped as soon as poll() or execute() sees its host
// disconnected. Host not connected -> Failed(AdapterUnavailable), never fake
// success.
namespace hubbo::integrations {

enum class HidPage : uint8_t { Keyboard, Consumer };

struct HidKey {
    const char *name;
    HidPage page;
    uint16_t usage;
};

size_t bleHidKeyCount();
const HidKey &bleHidKey(size_t index);  // index == the `key` param's option index
int bleHidKeyIndex(const char *name);   // -1 if unknown

enum class BleHidCommand : uint8_t { KeyDown, KeyUp, KeyTap };
CommandCatalog bleHidCatalog();  // indices match BleHidCommand

// The radio, per host: `deviceId` is the hub device whose bonded host gets
// the report (the firmware finds that host's live connection by its identity
// address at send time). Called on the engine task; never blocks.
class HidTransport {
public:
    virtual ~HidTransport() = default;
    // That device's host is bonded and connected.
    virtual bool connected(std::string_view deviceId) const = 0;
    // Keyboard input report: [modifiers, reserved, k1..k6].
    virtual bool sendKeyboard(std::string_view deviceId, const uint8_t report[8]) = 0;
    // Consumer control: one 16-bit usage, 0 = released.
    virtual bool sendConsumer(std::string_view deviceId, uint16_t usage) = 0;
};

class BleHidAdapter : public IntegrationAdapter {
public:
    BleHidAdapter(const char *integrationId, HidTransport &transport);

    const char *integrationId() const override { return id_; }
    const char *integrationType() const override;
    CommandCatalog commandsFor(const DeviceSpec &device) const override;
    ExecResult execute(const ExecRequest &request) override;
    void releaseAll(uint32_t nowMs) override;
    void poll(uint32_t nowMs, CompletionSink &sink) override;

    // What's held on one device's host.
    size_t heldKeyboardKeys(std::string_view deviceId) const;
    uint16_t heldConsumer(std::string_view deviceId) const;

private:
    // One host's report state. A slot is free when nothing is held.
    struct Host {
        IdString deviceId;
        uint8_t keys[6] = {};
        uint16_t consumer = 0;
        size_t keyCount() const;  // keyboard keys held
        bool holding() const;
    };

    ExecResult press(Host &host, const HidKey &key);
    ExecResult release(Host &host, const HidKey &key);
    bool flushKeyboard(const Host &host);
    void releaseHost(Host &host);  // send releases (if connected), then free the slot
    Host *find(std::string_view deviceId);
    const Host *find(std::string_view deviceId) const;
    Host *findOrAdd(std::string_view deviceId);  // nullptr: no free slot

    const char *id_;
    HidTransport &transport_;
    Host hosts_[limits::kMaxBleDevices];
};

}  // namespace hubbo::integrations
