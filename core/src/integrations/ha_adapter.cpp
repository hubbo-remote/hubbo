#include "hubbo/integrations/ha_adapter.h"

namespace hubbo::integrations {

HomeAssistantAdapter::HomeAssistantAdapter(const char *integrationId, HomeAssistantClient &client)
    : id_(integrationId), client_(client) {}

CommandCatalog HomeAssistantAdapter::commandsFor(const DeviceSpec &device) const {
    if (!device.haCapabilities) return {};
    const std::string entityId = providerRef(device);
    const ha::CapabilitySnapshot &snapshot = *device.haCapabilities;

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byDevice_.find(device.id);
    if (it != byDevice_.end() && it->second.entityId == entityId && it->second.snapshot == snapshot) {
        return it->second.capabilities->catalog();
    }
    // New or changed: new storage; the old catalog stays valid for compiled
    // configs that still use it.
    storage_.push_back(ha::EntityCapabilities::fromSnapshot(entityId, snapshot));
    byDevice_[device.id] = Cached{entityId, snapshot, &storage_.back()};
    return storage_.back().catalog();
}

const ha::EntityCapabilities *HomeAssistantAdapter::owner(const CommandSpec *command) const {
    for (const ha::EntityCapabilities &caps : storage_) {
        const CommandCatalog catalog = caps.catalog();
        if (catalog.count > 0 && command >= catalog.items && command < catalog.items + catalog.count) return &caps;
    }
    return nullptr;
}

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
        ha::ServiceCall serviceCall;
        ErrorCode built = ErrorCode::UnknownCommand;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const ha::EntityCapabilities *caps = owner(call.command);
            if (caps != nullptr && caps->entityId() == call.entityId.view()) {
                built = caps->buildServiceCall(call.commandIndex, call.params, call.paramsPresent, &serviceCall);
            }
        }
        if (built != ErrorCode::Ok) {
            finish(call.ticket, ErrorCode::AdapterFailed);
            continue;
        }
        const Ticket ticket = call.ticket;
        const ErrorCode sent = client_.callService(serviceCall, [this, ticket](ErrorCode result) {
            finish(ticket, result);
        });
        if (sent != ErrorCode::Ok) finish(ticket, ErrorCode::AdapterUnavailable);  // no queueing
    }
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
