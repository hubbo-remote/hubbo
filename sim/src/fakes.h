#pragma once

// Simulated integration adapters. They never touch hardware or the network:
// they record what they were asked to do. Host-only (hub-sim and tests).
//
//   RecordingHidTransport  HidTransport for the real core BleHidAdapter;
//                          records HID reports.
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
#include <map>
#include <string>
#include <vector>

#include "hubbo/domain/adapter.h"
#include "hubbo/ha/capabilities.h"
#include "hubbo/integrations/ble_hid.h"

namespace hubbo::sim {

class RecordingHidTransport : public integrations::HidTransport {
public:
    bool connected() const override { return connected_; }
    bool sendKeyboard(const uint8_t report[8]) override;
    bool sendConsumer(uint16_t usage) override;

    void setConnected(bool connected) { connected_ = connected; }
    std::vector<std::string> &reports() { return reports_; }

private:
    bool connected_ = true;
    std::vector<std::string> reports_;  // "keyboard 00 00 1e 00 00 00 00 00", "consumer 0x0042"
};

class FakeHomeAssistant : public IntegrationAdapter {
public:
    explicit FakeHomeAssistant(std::string integrationId = "ha_main") : id_(std::move(integrationId)) {}

    // Registers an entity as HA would describe it (features, source lists).
    // Devices whose provider ref names no registered entity get no commands.
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

    // HA service calls made, in order: "<domain>.<service> <entity> k=v".
    std::vector<std::string> &calls() { return calls_; }

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
    std::map<std::string, ha::EntityCapabilities> entities_;  // by entity id; stable storage
    uint32_t latencyMs_ = 0;
    bool available_ = true;
    std::vector<Failure> failures_;
    std::deque<Delayed> delayed_;
    std::vector<std::string> calls_;
};

class FakeIr : public IntegrationAdapter {
public:
    const char *integrationId() const override { return "ir"; }
    const char *integrationType() const override { return "ir"; }
    CommandCatalog commandsFor(const DeviceSpec &device) const override;
    ExecResult execute(const ExecRequest &request) override;
};

// "ble", "ha_main" (with the fixture entities) and "ir", registered in that
// order. Not copyable: the set points at its members.
struct FakeAdapters {
    FakeAdapters();
    FakeAdapters(const FakeAdapters &) = delete;
    FakeAdapters &operator=(const FakeAdapters &) = delete;

    RecordingHidTransport hid;
    integrations::BleHidAdapter ble{"ble", hid};
    FakeHomeAssistant homeAssistant{"ha_main"};
    FakeIr ir;
    AdapterSet set;
};

// "k=v k=v" for a compiled command's params, using its spec for names, enum
// option strings and formatting. Optional params not in `present` are left
// out.
std::string formatParams(const CommandSpec &command, const ParamValue *params, uint8_t present);

}  // namespace hubbo::sim
