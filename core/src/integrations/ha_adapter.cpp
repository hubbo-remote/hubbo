#include "hubbo/integrations/ha_adapter.h"

#include <optional>
#include <string>

namespace hubbo::integrations {

HomeAssistantAdapter::HomeAssistantAdapter(const char *integrationId, HomeAssistantClient &client)
    : id_(integrationId), client_(client) {}

ExecResult HomeAssistantAdapter::execute(const ExecRequest &request) {
    // Engine task: never waits. Not connected, or the hand-over is full: fail
    // now rather than queue an action for later.
    if (client_.state() != HaState::Connected) return ExecResult::failed(ErrorCode::AdapterUnavailable);
    Call call;
    call.ticket = request.ticket;
    call.command = request.command;
    call.commandIndex = request.commandIndex;
    const uint8_t count = request.command != nullptr ? request.command->paramCount : 0;
    for (uint8_t i = 0; i < count && i < limits::kMaxParams; i++) call.params[i] = request.params[i];
    call.paramsPresent = request.paramsPresent;
    if (!call.entityId.assign(request.providerRef)) return ExecResult::failed(ErrorCode::AdapterFailed);
    call.custom = request.call;
    if (!calls_.push(call)) return ExecResult::failed(ErrorCode::AdapterUnavailable);
    return ExecResult::pending();
}

void HomeAssistantAdapter::finish(Ticket ticket, ErrorCode error) {
    Result r;
    r.ticket = ticket;
    r.result = error == ErrorCode::Ok ? ExecResult::accepted() : ExecResult::failed(error);
    // Full only if the engine stopped polling; the executor's timeout then
    // reports it.
    results_.push(r);
}

void HomeAssistantAdapter::service() {
    Call call;
    while (calls_.pop(&call)) {
        // HA answers success for an entity it doesn't have, so a stale one
        // (missing from the latest discovery) is refused here, unsent.
        if (client_.entityStale(call.entityId.view())) {
            finish(call.ticket, ErrorCode::EntityNotFound);
            continue;
        }
        const ha::EntityCapabilities *caps = catalogs_.owner(call.command, call.entityId.view());
        if (caps == nullptr) {
            finish(call.ticket, ErrorCode::AdapterFailed);
            continue;
        }
        const char *attribute = call.custom ? nullptr : caps->stateAttribute(call.commandIndex);
        if (attribute == nullptr) {
            send(call, *caps, nullptr, HomeAssistantClient::kCommandTimeoutMs);
            continue;
        }
        // Read the one attribute fresh, then send. Each press reads: two
        // quick presses may both see the old state (accepted).
        const uint64_t readAt = client_.nowMs();
        const ErrorCode started = client_.readAttribute(
            std::string(call.entityId.view()), attribute,
            [this, call, caps, readAt](ErrorCode result, std::optional<ha::ServiceValue> value) {
                if (result == ErrorCode::EntityNotFound || result == ErrorCode::AdapterUnavailable) {
                    finish(call.ticket, result);  // no such entity, or no connection to send on
                    return;
                }
                // The read and the call share kCommandTimeoutMs.
                const uint64_t spent = client_.nowMs() - readAt;
                if (spent >= HomeAssistantClient::kCommandTimeoutMs) return finish(call.ticket, ErrorCode::AdapterTimeout);
                // A failed or timed-out read still sends, as unknown.
                send(call, *caps, result == ErrorCode::Ok && value ? &*value : nullptr,
                     static_cast<uint32_t>(HomeAssistantClient::kCommandTimeoutMs - spent));
            });
        if (started != ErrorCode::Ok) finish(call.ticket, ErrorCode::AdapterUnavailable);  // no queueing
    }
}

void HomeAssistantAdapter::send(const Call &call, const ha::EntityCapabilities &capabilities,
                                const ha::ServiceValue *state, uint32_t timeoutMs) {
    ha::ServiceCall serviceCall;
    const ErrorCode built = HaCatalogs::buildCall(capabilities, call.commandIndex, call.params, call.paramsPresent,
                                                  call.custom.get(), state, &serviceCall);
    if (built != ErrorCode::Ok) return finish(call.ticket, ErrorCode::AdapterFailed);
    const Ticket ticket = call.ticket;
    const ErrorCode sent = client_.callService(
        serviceCall, [this, ticket](ErrorCode result) { finish(ticket, result); }, timeoutMs);
    if (sent != ErrorCode::Ok) finish(ticket, ErrorCode::AdapterUnavailable);  // no queueing
}

const char *haDeviceAvailability(const HomeAssistantClient &client, const DeviceSpec &device) {
    if (client.entityStale(device.provider.entityId)) return "stale";
    return client.state() == HaState::Connected ? "available" : "unavailable";
}

const char *haIntegrationState(const HomeAssistantClient &client, const IntegrationSpec &integration) {
    return integration.enabled ? haStateName(client.state()) : "disabled";
}

void HomeAssistantAdapter::poll(uint32_t nowMs, CompletionSink &sink) {
    Result r;
    while (results_.pop(&r)) sink.complete(r.ticket, r.result, nowMs);
}

}  // namespace hubbo::integrations
