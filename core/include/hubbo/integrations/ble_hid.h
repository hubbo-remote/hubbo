#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include "hubbo/domain/adapter.h"
#include "hubbo/domain/ids.h"
#include "hubbo/domain/limits.h"
#include "hubbo/domain/params.h"

// The Bluetooth keyboard integration: each device is a host paired with the
// hub as a keyboard (docs/design/multi-bluetooth.md), and carries the
// profile it was added from (docs/design/device-profiles.md).
//
// Commands for a device, in three groups (CommandSpec::group):
//   remote  press {command, hold_ms?}   the profile's command by name; a tap,
//                                       or a timed press with hold_ms (the
//                                       step's, else the command's own)
//           press_down / press_up {command}   hold and release
//   keys    key_tap / key_down / key_up {key}   any key in the table below:
//                                       keyboard 0x04-0x65 and the named
//                                       consumer keys
//   custom  usage_tap / usage_down / usage_up {usage}   a raw consumer usage
//                                       (limits::kHidConsumerUsage*)
//   (none)  wait_connected {timeout_ms?}   park a startup or shutdown until
//                                       the host is connected (effect Wait).
//                                       Default 15 s; on a timeout it
//                                       completes Accepted (logged as
//                                       adapter_unavailable) and the sequence
//                                       goes on.
//
// Each device is its own host: a key goes to the host of the device its
// action names, never elsewhere. BleHidAdapter holds the report state per
// device (6-key keyboard rollover plus one consumer usage) and talks to the
// radio through HidTransport, which the firmware implements with NimBLE and
// the simulator records. Keys a binding holds are released on releaseAll()
// (activity switch, lost release); a timed press or a wait ends with its
// time, or on cancel() when its sequence is cancelled. A device's held keys
// are dropped as soon as poll() or execute() sees its host disconnected.
// A directly executed timed press (ExecRequest::answerAtStart) answers at
// key-down and releases by itself. Host not connected ->
// Failed(AdapterUnavailable), never fake success.
namespace hubbo::integrations {

using hubbo::HidPage;

struct HidKey {
    const char *name;
    HidPage page;
    uint16_t usage;
};

// The All keys table (the `key` param's options, in order).
size_t bleHidKeyCount();
const HidKey &bleHidKey(size_t index);  // index == the `key` param's option index
int bleHidKeyIndex(const char *name);   // -1 if unknown

// Indices in every Bluetooth device's catalog.
enum class BleHidCommand : uint8_t {
    Press, PressDown, PressUp,
    KeyTap, KeyDown, KeyUp,
    UsageTap, UsageDown, UsageUp,
    WaitConnected,
};
constexpr uint32_t kBleWaitConnectedDefaultMs = 15000;
constexpr uint32_t kBleWaitConnectedMinMs = 1000;
constexpr uint32_t kBleWaitConnectedMaxMs = 60000;

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
    ~BleHidAdapter() override;

    const char *integrationId() const override { return id_; }
    const char *integrationType() const override;
    // The device's catalog (BleHidCommand order); its `command` options are
    // the profile's command names. Empty without a profile. Config task;
    // the storage outlives every compiled config (like the HA adapter's).
    CommandCatalog commandsFor(const DeviceSpec &device) const override;
    ExecResult execute(const ExecRequest &request) override;
    void releaseAll(uint32_t nowMs) override;
    void cancel(Ticket ticket, uint32_t nowMs) override;
    void poll(uint32_t nowMs, CompletionSink &sink) override;

    // What's held on one device's host.
    size_t heldKeyboardKeys(std::string_view deviceId) const;
    uint16_t heldConsumer(std::string_view deviceId) const;

private:
    struct DeviceCatalog;  // one device's commands (CommandSpec::data)

    // A timed press waiting for its release, or a wait for a connection.
    struct Timer {
        Ticket ticket = 0;     // 0 is a valid ticket (a forced release's)
        IdString deviceId;     // empty: a free slot
        HidKey key{};          // timed press: the key to release
        bool waitConnected = false;
        bool detached = false; // answered at the start (direct execute): finish silently
        uint32_t atMs = 0;     // release, or give up waiting
        bool inUse() const { return !deviceId.empty(); }
    };
    static constexpr size_t kMaxTimers = limits::kMaxBleDevices * 2;

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
    static void freeIfIdle(Host &host);  // free the slot once nothing is held
    Host *find(std::string_view deviceId);
    const Host *find(std::string_view deviceId) const;
    Host *findOrAdd(std::string_view deviceId);  // nullptr: no free slot

    ExecResult start(const ExecRequest &request, bool waitConnected, const HidKey &key, uint32_t atMs);
    void finishTimer(Timer &t);  // releases a timed press's key, frees the slot
    bool timed(const Host &host, HidPage page, uint16_t usage) const;  // a timer holds it

    const char *id_;
    HidTransport &transport_;
    Host hosts_[limits::kMaxBleDevices];
    Timer timers_[kMaxTimers];
    // Catalogs built by commandsFor() (config task), one per distinct
    // command list (so per profile, not per edit), never freed while the
    // adapter lives: compiled configs point into them.
    mutable std::mutex catalogsMutex_;
    mutable std::vector<std::unique_ptr<DeviceCatalog>> catalogs_;
};

}  // namespace hubbo::integrations
