#include "hubbo/api/transitions.h"

#include <memory>

#include "hubbo/api/api.h"
#include "hubbo/config/service.h"

namespace hubbo::api {

TransitionOutcome runTransition(const config::ConfigService &config, Runtime &runtime, std::string_view activityId) {
    TransitionOutcome outcome;
    // A blocked hub runs no configuration. (storage_unavailable doesn't stop
    // switching; the active activity just isn't saved.)
    if (config.state() == config::ConfigState::Blocked) {
        outcome.status = TransitionStatus::Blocked;
        return outcome;
    }
    auto request = std::make_unique<EngineRequest>();
    if (activityId.empty()) {
        request->off();
    } else {
        request->activate(activityId);
    }
    if (!runtime.run(*request)) return outcome;  // Busy
    outcome.status = TransitionStatus::Done;
    outcome.result = request->transition();
    return outcome;
}

RestartOutcome requestHubRestart(RestartPort *restart, const FirmwareUpdatePort *firmware) {
    if (restart == nullptr) return RestartOutcome::NoPort;
    if (firmware != nullptr && firmwareUpdateRunning(firmware->status().state)) return RestartOutcome::UpdateInProgress;
    return restart->requestRestart() ? RestartOutcome::Accepted : RestartOutcome::Unavailable;
}

}  // namespace hubbo::api
