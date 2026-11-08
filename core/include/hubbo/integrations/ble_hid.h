#pragma once

#include <cstddef>
#include <cstdint>

#include "hubbo/domain/adapter.h"
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
// BleHidAdapter holds the report state (6-key keyboard rollover plus one
// consumer usage) and talks to the radio through HidTransport, which the
// firmware implements with NimBLE and the simulator records. Held keys are
// released on releaseAll() (activity switch, lost release) and dropped as
// soon as poll() or execute() sees the host disconnected. Not connected -> Failed(AdapterUnavailable), never fake
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

class HidTransport {
public:
    virtual ~HidTransport() = default;
    virtual bool connected() const = 0;
    // Keyboard input report: [modifiers, reserved, k1..k6]. Must not block.
    virtual bool sendKeyboard(const uint8_t report[8]) = 0;
    // Consumer control: one 16-bit usage, 0 = released. Must not block.
    virtual bool sendConsumer(uint16_t usage) = 0;
};

class BleHidAdapter : public IntegrationAdapter {
public:
    BleHidAdapter(const char *integrationId, HidTransport &transport);

    const char *integrationId() const override { return id_; }
    CommandCatalog commandsFor(const DeviceSpec &device) const override;
    ExecResult execute(const ExecRequest &request) override;
    void releaseAll(uint32_t nowMs) override;
    void poll(uint32_t nowMs, CompletionSink &sink) override;

    size_t heldKeyboardKeys() const;
    uint16_t heldConsumer() const { return consumer_; }

private:
    ExecResult press(const HidKey &key);
    ExecResult release(const HidKey &key);
    bool flushKeyboard();
    bool forgetIfDisconnected();  // true (and held state cleared) if not connected
    void clearHeld();

    const char *id_;
    HidTransport &transport_;
    uint8_t keys_[6] = {};
    uint16_t consumer_ = 0;
};

}  // namespace hubbo::integrations
