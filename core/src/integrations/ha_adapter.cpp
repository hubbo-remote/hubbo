#include "hubbo/integrations/ha_adapter.h"

#include <optional>
#include <string>

#include "hubbo/integrations/state_check.h"

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
    call.checkState = request.checkState;
    if (!calls_.push(call)) return ExecResult::failed(ErrorCode::AdapterUnavailable);
    return ExecResult::pending();
}

void HomeAssistantAdapter::cancel(Ticket ticket, uint32_t nowMs) {
    (void)nowMs;
    cancels_.push(ticket);
}

bool HomeAssistantAdapter::cancelled(Ticket ticket) {
    Ticket t = 0;
    while (cancels_.pop(&t)) {
        cancelled_[cancelledNext_] = t;
        cancelledNext_ = (cancelledNext_ + 1) % (sizeof cancelled_ / sizeof cancelled_[0]);
    }
    for (const Ticket c : cancelled_) {
        if (c == ticket && c != 0) return true;
    }
    return false;
}

void HomeAssistantAdapter::finish(Ticket ticket, ExecResult result) {
    // Full only if the engine stopped polling; the executor's timeout then
    // reports it.
    results_.push(Result{ticket, result});
}

void HomeAssistantAdapter::finish(Ticket ticket, ErrorCode error) {
    finish(ticket, error == ErrorCode::Ok ? ExecResult::accepted() : ExecResult::failed(error));
}

void HomeAssistantAdapter::service() {
    Call call;
    while (calls_.pop(&call)) {
        if (cancelled(call.ticket)) continue;  // the executor has forgotten it
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
            ha::ServiceCall built;
            if (!build(call, *caps, nullptr, &built)) continue;
            if (call.checkState && checkable(built)) {
                check(call.ticket, built);
            } else {
                send(call.ticket, built);
            }
            continue;
        }
        // Read the one attribute fresh, then send. Each press reads: two
        // quick presses may both see the old state (accepted). A command
        // that acts on the current state is never checked: it sets no state.
        const ErrorCode started = client_.readAttribute(
            std::string(call.entityId.view()), attribute,
            [this, call, caps](ErrorCode result, std::optional<ha::ServiceValue> value) {
                if (result == ErrorCode::EntityNotFound || result == ErrorCode::AdapterUnavailable) {
                    finish(call.ticket, result);  // no such entity, or no connection to send on
                    return;
                }
                // A failed or timed-out read still sends, as unknown (a toggle
                // mutes; a step refuses: StateUnavailable).
                ha::ServiceCall built;
                if (build(call, *caps, result == ErrorCode::Ok && value ? &*value : nullptr, &built)) {
                    send(call.ticket, built);
                }
            });
        if (started != ErrorCode::Ok) finish(call.ticket, ErrorCode::AdapterUnavailable);  // no queueing
    }
}

bool HomeAssistantAdapter::build(const Call &call, const ha::EntityCapabilities &capabilities,
                                 const ha::ServiceValue *state, ha::ServiceCall *out) {
    const ErrorCode built = HaCatalogs::buildCall(capabilities, call.commandIndex, call.params, call.paramsPresent,
                                                  call.custom.get(), state, out);
    if (built == ErrorCode::Ok) return true;
    // A stepped command with no current state (a cover's unknown position)
    // sends nothing and says why; anything else is the adapter's fault.
    finish(call.ticket, built == ErrorCode::StateUnavailable ? built : ErrorCode::AdapterFailed);
    return false;
}

void HomeAssistantAdapter::check(Ticket ticket, const ha::ServiceCall &call) {
    // A fresh read of the one entity; anything but a clear "already set"
    // within kStateCheckMs (a failed or slow read, an unavailable or
    // unknown entity) sends the step as usual.
    const ErrorCode started =
        client_.readState(call.entityId, kStateCheckMs, [this, ticket, call](ErrorCode result, std::string_view entity) {
            if (result == ErrorCode::Ok && alreadySet(call, entity)) return finish(ticket, ExecResult::skipped());
            send(ticket, call);
        });
    if (started != ErrorCode::Ok) send(ticket, call);
}

void HomeAssistantAdapter::send(Ticket ticket, const ha::ServiceCall &call) {
    // Cancelled while its state was read (an Off right after an activate):
    // never sent, so the shutdown that replaced it checks what's really there.
    if (cancelled(ticket)) return;
    const ErrorCode sent = client_.callService(
        call,
        [this, ticket](ErrorCode result) {
            // No answer within kReplyLimitMs is no reply, not a failure: HA
            // may well have acted (some integrations answer only after their
            // own timeout). The client forgets the call: a late reply is dropped.
            if (result == ErrorCode::AdapterTimeout) return finish(ticket, ExecResult::noReply());
            finish(ticket, result);
        },
        kReplyLimitMs);
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
