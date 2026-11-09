#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>

#include "hubbo/domain/adapter.h"
#include "hubbo/domain/ids.h"
#include "hubbo/domain/limits.h"
#include "hubbo/engine/spsc.h"
#include "hubbo/ha/capabilities.h"
#include "hubbo/integrations/ha_client.h"

// The Home Assistant adapter for "ha_main" (docs/design/phase3-home-
// assistant.md, section 2). Three tasks touch it, none waits for another:
//
//   engine task       execute(): AdapterUnavailable at once unless the client
//                     is Connected and the hand-over ring has room; else the
//                     call goes to the integration task through that
//                     lock-free ring and the result is Pending. poll():
//                     delivers results to the executor's completion sink.
//                     Never locks, never allocates.
//   integration task  service(): takes handed-over calls, refuses a stale
//                     entity (EntityNotFound, unsent: HA would answer
//                     success), builds each HA service call (ha/
//                     capabilities: the registered entity is the only
//                     target) and sends it; HA's result (or the
//                     client's kCommandTimeoutMs) comes back through a second
//                     ring as Accepted or Failed. A call that can't be sent
//                     now fails; nothing is queued for later or replayed
//                     after a reconnect.
//   config task       commandsFor() (config compile): the device's stored
//                     capabilities snapshot, so HA isn't needed to compile.
//
// The executor's own action timeout (limits::kDefaultActionTimeoutMs, 5 s)
// bounds every Pending result; a later result is ignored.
namespace hubbo::integrations {

class HomeAssistantAdapter : public IntegrationAdapter {
public:
    static constexpr size_t kQueueDepth = 16;

    HomeAssistantAdapter(const char *integrationId, HomeAssistantClient &client);

    const char *integrationId() const override { return id_; }
    const char *integrationType() const override { return kHomeAssistantType; }

    // Config task. From the device's capabilities snapshot, cached per
    // device: a changed snapshot gets new storage and the old one stays
    // alive until the adapter is destroyed (a reboot). Its CommandSpecs are
    // pointed at from more than compiled configs: queued and in-flight
    // executor work and held keys survive a config publish, an
    // EngineRequest carries a CompiledCommand, a Call carries the command.
    // So growth is one set per capabilities change (a device re-registered
    // after HA changed it), not per compile. Empty without a snapshot (the
    // device can't be targeted).
    CommandCatalog commandsFor(const DeviceSpec &device) const override;

    // Engine task.
    ExecResult execute(const ExecRequest &request) override;
    void poll(uint32_t nowMs, CompletionSink &sink) override;

    // Integration task, every loop (after client.poll()).
    void service();

private:
    struct Call {
        Ticket ticket = 0;
        const CommandSpec *command = nullptr;  // in a catalog this adapter keeps alive
        uint16_t commandIndex = 0;
        ParamValue params[limits::kMaxParams];
        uint8_t paramsPresent = 0;
        FixedString<limits::kMaxProviderRefLength> entityId;
    };
    struct Result {
        Ticket ticket = 0;
        ExecResult result;
    };
    struct Cached {
        // What the capabilities were built from.
        std::string entityId;
        ha::CapabilitySnapshot snapshot;
        const ha::EntityCapabilities *capabilities = nullptr;
    };

    // The capabilities `command` belongs to (integration task).
    const ha::EntityCapabilities *owner(const CommandSpec *command) const;
    void finish(Ticket ticket, ErrorCode error);

    const char *id_;
    HomeAssistantClient &client_;
    SpscRing<Call, kQueueDepth + 1> calls_;          // engine -> integration
    SpscRing<Result, 2 * kQueueDepth + 1> results_;  // integration -> engine

    mutable std::mutex mutex_;  // config task (commandsFor) vs integration task (service)
    mutable std::map<std::string, Cached, std::less<>> byDevice_;
    mutable std::deque<ha::EntityCapabilities> storage_;  // stable addresses, never shrinks (see commandsFor)
};

// A home_assistant device's availability (api::Runtime::deviceAvailability):
// "stale" (missing from the latest discovery), else "available" while
// connected, else "unavailable". The firmware and hub-sim share it.
const char *haDeviceAvailability(const HomeAssistantClient &client, const DeviceSpec &device);

// The home_assistant integration's state (api::Runtime::integrationState):
// "disabled" when it is (the client then has no URL and says
// not_configured), else the client's (haStateName()). The firmware and
// hub-sim share it.
const char *haIntegrationState(const HomeAssistantClient &client, const IntegrationSpec &integration);

}  // namespace hubbo::integrations
