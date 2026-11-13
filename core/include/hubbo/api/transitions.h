#pragma once

#include <cstdint>
#include <string_view>

#include "hubbo/engine/engine.h"

// Activity transitions and the hub restart, shared by the HTTP routes and
// the MQTT bridge's hooks so both obey the same rules.
namespace hubbo::config {
class ConfigService;
}

namespace hubbo::api {

class Runtime;
class RestartPort;
class FirmwareUpdatePort;

enum class TransitionStatus : uint8_t {
    Done,     // reached the engine; see `result`
    Busy,     // the engine's mailbox is full; nothing happened (HTTP 503 busy)
    Blocked,  // the configuration is blocked; nothing ran (HTTP 409 config_blocked)
};

struct TransitionOutcome {
    TransitionStatus status = TransitionStatus::Busy;
    // Done only. `!switched && error != NoActiveActivity` is a refusal
    // (UnknownActivity); off with nothing active is a no-op.
    TransitionResult result;
};

// Activates `activityId`, or turns the hub off for "". Waits until the
// engine is done (Runtime::run). Any task but the engine's; the request is
// heap-allocated, so it suits the 16 KB httpd stack.
TransitionOutcome runTransition(const config::ConfigService &config, Runtime &runtime, std::string_view activityId);

enum class RestartOutcome : uint8_t {
    Accepted,          // the hub reboots shortly (HTTP 202)
    NoPort,            // this platform can't restart at all (HTTP 503 "This hub can't restart")
    Unavailable,       // the port refused for now (HTTP 503 "The hub can't restart right now")
    UpdateInProgress,  // a firmware update is writing or rebooting (HTTP 409)
};

// POST /admin/restart's rule. Either pointer may be null.
RestartOutcome requestHubRestart(RestartPort *restart, const FirmwareUpdatePort *firmware);

}  // namespace hubbo::api
