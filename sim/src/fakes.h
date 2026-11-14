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
//   FakeIr                 IR placeholder. IR is post-MVP, so it reports
//                          NotImplemented (no fake success), but its catalog
//                          lets configs reference IR devices.
//   FakeAdapters           all of the above, registered in one AdapterSet.

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "hubbo/domain/adapter.h"
#include "hubbo/ha/capabilities.h"
#include "hubbo/integrations/ble_hid.h"
#include "hubbo/integrations/ble_hosts.h"
#include "hubbo/integrations/ha_catalogs.h"

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
    // become its attributes (volume_mute sets is_volume_muted), which a
    // command with a stateAttribute (mute_toggle) reads. Unset: unknown.
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

class FakeIr : public IntegrationAdapter {
public:
    const char *integrationId() const override { return "ir"; }
    const char *integrationType() const override { return "ir"; }
    CommandCatalog commandsFor(const DeviceSpec &device) const override;
    ExecResult execute(const ExecRequest &request) override;
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
    FakeIr ir;
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
