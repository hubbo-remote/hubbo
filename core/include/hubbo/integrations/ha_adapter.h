#pragma once

#include <cstddef>
#include <cstdint>

#include "hubbo/domain/adapter.h"
#include "hubbo/domain/ids.h"
#include "hubbo/domain/limits.h"
#include "hubbo/engine/spsc.h"
#include "hubbo/ha/capabilities.h"
#include "hubbo/integrations/ha_catalogs.h"
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
//                     target) and sends it. A command that acts on the
//                     current state (stateAttribute(), e.g. a mute toggle)
//                     first reads that one attribute fresh
//                     (client.readAttribute; no cache, no standing
//                     subscription), then builds the call from it and
//                     sends it with what's left of kCommandTimeoutMs, so
//                     the read and the call fit the 5 s together (a
//                     Home Assistant action, call_service, is sent as the
//                     user wrote it: buildCustomCall); a
//                     failed or timed-out read still sends, as unknown; an
//                     entity HA has no state for fails EntityNotFound; HA's result (or the
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

    // Config task. From the device's capabilities snapshot (HaCatalogs:
    // cached per device, kept until a reboot). Empty without a snapshot.
    CommandCatalog commandsFor(const DeviceSpec &device) const override { return catalogs_.commandsFor(device); }

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
        HaServiceCallRef custom;  // call_service; kept until the call is built
    };
    struct Result {
        Ticket ticket = 0;
        ExecResult result;
    };
    void finish(Ticket ticket, ErrorCode error);
    // Integration task: build `call` from `capabilities` (with `state`, the
    // value read for a command that needs one) and send it with `timeoutMs`;
    // the ticket finishes with HA's result.
    void send(const Call &call, const ha::EntityCapabilities &capabilities, const ha::ServiceValue *state,
              uint32_t timeoutMs);

    const char *id_;
    HomeAssistantClient &client_;
    SpscRing<Call, kQueueDepth + 1> calls_;          // engine -> integration
    SpscRing<Result, 2 * kQueueDepth + 1> results_;  // integration -> engine
    mutable HaCatalogs catalogs_;  // config task (commandsFor) vs integration task (service)
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
