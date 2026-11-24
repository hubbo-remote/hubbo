#pragma once

// Simulated integration adapters. They never touch hardware or the network:
// they record what they were asked to do. Host-only (hub-sim and tests).
//
//   SimulatedBleHosts      several Bluetooth hosts, each bonded to a hub
//                          device: the API's BleHostPort and the HidTransport
//                          for the real core BleHidAdapter; records reports.
//   FakeHomeAssistant      catalogs from the real ha::EntityCapabilities for
//                          registered fake entities; records the HA service
//                          call each command would make. Immediate or delayed
//                          (Pending) results, injectable failures.
//   FakeIrTransmitter      the IR emitter for the real core IrAdapter: keeps
//                          each transmission busy for as long as its frames
//                          take on hub time, and records what was sent.
//   FakeAdapters           all of the above, registered in one AdapterSet.

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "hubbo/domain/adapter.h"
#include "hubbo/ha/capabilities.h"
#include "hubbo/integrations/ble_hid.h"
#include "hubbo/integrations/ble_hosts.h"
#include "hubbo/integrations/ha_catalogs.h"
#include "hubbo/integrations/ir.h"
#include "hubbo/integrations/ir_transmitter.h"

namespace hubbo::sim {

// The hub's Bluetooth radio and its hosts (docs/design/multi-bluetooth.md).
// A host is bonded to one hub device; up to kMaxBleDevices bonds. Single task.
class SimulatedBleHosts : public integrations::BleHostPort, public integrations::HidTransport {
public:
    // BleHostPort
    bool radioReady() const override { return ready; }
    integrations::BleHostStatus status(std::string_view deviceId) const override;
    integrations::BleHostResult startPairing(std::string_view deviceId, uint32_t windowMs) override;
    integrations::BleHostResult cancelPairing(std::string_view deviceId) override;
    integrations::BleHostResult forget(std::string_view deviceId) override;
    void retainOnly(const integrations::BleDeviceIds &devices) override;  // applied at once

    // HidTransport: a report reaches only that device's connected host.
    bool connected(std::string_view deviceId) const override;
    bool sendKeyboard(std::string_view deviceId, const uint8_t report[8]) override;
    bool sendConsumer(std::string_view deviceId, uint16_t usage) override;

    // Simulation controls.
    void advance(uint32_t elapsedMs);  // the pairing window's timeout
    // A host named `hostName` bonds during the open window: it becomes the
    // window's device's host (replacing its old one), connected. False (and
    // nothing changes) with no window or a host already bonded to another
    // device; a full bond store closes the window (keeping what was there).
    bool bonded(const std::string &hostName);
    // A bonded host's link (false if `deviceId` has no host).
    bool setConnected(std::string_view deviceId, bool connected);
    void setAllConnected(bool connected);  // every bonded host
    // Bonds `deviceId` to `hostName` directly (a fixture world), connected.
    void bond(const std::string &deviceId, const std::string &hostName);
    // No bonds, no window, the radio working. Reports stay.
    void reset();
    // The bonds as text ("<device id>\t<host name>" lines), as the board
    // keeps them in NVS; loadBonds() replaces the bonds with them (each host
    // connected). False on a malformed line (nothing changes).
    std::string saveBonds() const;
    bool loadBonds(const std::string &text);
    std::function<void()> onBondsChanged;  // after any bond is added or removed
    bool ready = true;                     // false: the radio isn't working

    // Every report sent, in order: "keyboard 00 00 1e 00 00 00 00 00",
    // "consumer 0x0042", and the device whose host got each.
    std::vector<std::string> &reports() { return reports_; }
    const std::vector<std::string> &reportDevices() const { return reportDevices_; }

private:
    struct Host {
        std::string deviceId;
        std::string name;
        bool connected = true;
    };
    Host *host(std::string_view deviceId);
    const Host *host(std::string_view deviceId) const;
    void record(std::string_view deviceId, std::string report);
    void bondsChanged();
    void setHost(const std::string &deviceId, const std::string &hostName);  // replace or append, connected
    template <typename Pred>
    void eraseHosts(Pred gone);

    std::vector<Host> hosts_;  // bonded, at most kMaxBleDevices
    std::string windowDevice_;  // "" when no window is open
    uint32_t windowRemainingMs_ = 0;
    std::vector<std::string> reports_;
    std::vector<std::string> reportDevices_;
};

// A fake cover's current_position until something moves it (both fakes).
constexpr int32_t kFakeCoverPosition = 50;

class FakeHomeAssistant : public IntegrationAdapter {
public:
    explicit FakeHomeAssistant(std::string integrationId = "ha_main") : id_(std::move(integrationId)) {}

    // Registers an entity as HA would describe it (features, source lists).
    // A device with a stored snapshot gets the commands it describes, as from
    // the real adapter (custom services, so call_service, included); else its
    // entity's, and none if no registered entity has its provider ref.
    void addEntity(const ha::EntityInfo &entity);

    const char *integrationId() const override { return id_.c_str(); }
    const char *integrationType() const override { return kHomeAssistantType; }
    CommandCatalog commandsFor(const DeviceSpec &device) const override;
    ExecResult execute(const ExecRequest &request) override;
    void poll(uint32_t nowMs, CompletionSink &sink) override;

    // Results arrive after `ms` as Pending completions (0 = immediate).
    void setLatency(uint32_t ms) { latencyMs_ = ms; }
    // The next execution of `commandId` fails with `error` (or times out).
    void failNext(std::string commandId, ErrorCode error);
    void setAvailable(bool available) { available_ = available; }
    bool available() const { return available_; }
    // Back to defaults: no latency, no injected failures, available.
    void resetFaults() {
        latencyMs_ = 0;
        available_ = true;
        failures_.clear();
    }

    // HA service calls made, in order: "<domain>.<service> <entity> k=v",
    // and a Home Assistant action as "<service> <entity> <data json>".
    std::vector<std::string> &calls() { return calls_; }
    // An entity's state as the calls left it: each call's data fields
    // become its attributes (volume_mute sets is_volume_muted;
    // set_cover_position also sets current_position), which a command with a
    // stateAttribute (mute_toggle, position_step) reads. Unset: unknown. A
    // cover starts at kFakeCoverPosition.
    const ha::ServiceValue *attribute(const std::string &entityId, const std::string &key) const;

private:
    struct Delayed {
        Ticket ticket;
        uint32_t dueMs;
        ExecResult result;
    };
    struct Failure {
        std::string commandId;
        ErrorCode error;
    };

    std::string id_;
    // Every catalog handed out, as the real adapter keeps them (config task
    // and engine task).
    mutable integrations::HaCatalogs catalogs_;
    std::map<std::string, const ha::EntityCapabilities *> entities_;  // addEntity's, by entity id (in catalogs_)
    uint32_t latencyMs_ = 0;
    bool available_ = true;
    std::vector<Failure> failures_;
    std::deque<Delayed> delayed_;
    std::vector<std::string> calls_;
    std::map<std::string, std::map<std::string, ha::ServiceValue>> state_;  // entity -> key -> value
};

// One transmission, as the emitter saw it.
struct IrSend {
    uint32_t atMs = 0;
    std::string deviceId, command;
    uint32_t carrierHz = 0;
    uint16_t minFrames = 0;  // what send() asked for
    bool hold = false;
    std::optional<uint32_t> stoppedAtMs;  // stop() during a hold
    // Frames sent: all of them once ended, else those started so far.
    uint32_t frames = 0;
    std::optional<uint32_t> endedAtMs;  // after the last frame's gap
};

// Single task (the engine's). Time is `clock` (hub time in SimHub; a test
// sets its own).
class FakeIrTransmitter : public integrations::IrTransmitter {
public:
    static constexpr size_t kKept = 32;  // recent sends, oldest dropped

    bool ready() const override { return ready_; }
    bool busy() const override;
    bool send(const integrations::IrSignal &signal, uint16_t frames, bool hold) override;
    void stop() override;
    bool takeFailed() override;

    // As the board does: the next repeat frame is queued this long before
    // the one in flight ends, so a stop() then sends one more frame.
    static constexpr uint64_t kQueuedAheadUs = 5000;

    void setReady(bool ready) { ready_ = ready; }
    // The next send() is accepted, then fails (takeFailed() once it ends).
    void failNextSend() { failNext_ = true; }
    std::function<uint32_t()> clock = [] { return 0u; };
    // Oldest first; the last may still be sending (see IrSend).
    std::vector<IrSend> sends() const;
    void reset();  // ready, nothing sent

private:
    // A send and its timing; what it has sent is worked out when read.
    struct Record {
        IrSend send;  // frames and endedAtMs left unset
        uint64_t startUs = 0, firstUs = 0, repeatUs = 0;
        std::optional<uint64_t> stopUs;
        bool failed = false;  // reported by takeFailed() (then cleared)
    };
    uint64_t nowUs() const { return static_cast<uint64_t>(clock()) * 1000; }
    static uint32_t framesBy(const Record &r, uint64_t atUs);  // frames started by then
    static std::optional<uint64_t> endUs(const Record &r);     // none: a hold still running
    bool sending(const Record &r) const;

    bool ready_ = true;
    bool failNext_ = false;
    std::deque<Record> records_;  // the last one may be sending
};

// "ble", "ha_main" (with the fixture entities) and "ir", registered in that
// order. `realHomeAssistant`, if given, is registered as "ha_main" instead of
// the scripted one. Not copyable: the set points at its members.
struct FakeAdapters {
    explicit FakeAdapters(IntegrationAdapter *realHomeAssistant = nullptr);
    FakeAdapters(const FakeAdapters &) = delete;
    FakeAdapters &operator=(const FakeAdapters &) = delete;

    SimulatedBleHosts bleHosts;
    integrations::BleHidAdapter ble{"ble", bleHosts};
    FakeHomeAssistant homeAssistant{"ha_main"};
    FakeIrTransmitter irTransmitter;
    integrations::IrAdapter ir{"ir", irTransmitter};
    AdapterSet set;
};

// "k=v k=v" for a compiled command's params, using its spec for names, enum
// option strings and formatting. Optional params not in `present` are left
// out.
std::string formatParams(const CommandSpec &command, const ParamValue *params, uint8_t present);
// The pieces of the "k=v" log format: "%g" numbers, values with spaces quoted.
std::string formatNumber(double v);
std::string quoteIfSpaced(const std::string &s);

}  // namespace hubbo::sim
