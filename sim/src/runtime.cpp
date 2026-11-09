#include "runtime.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>

namespace hubbo::sim {

bool HostRandom::fill(uint8_t *out, size_t length) {
    std::random_device device;
    for (size_t i = 0; i < length; i++) out[i] = static_cast<uint8_t>(device());
    return true;
}

int64_t SimClock::unixSeconds() const { return static_cast<int64_t>(std::time(nullptr)); }

bool SimRuntime::run(EngineRequest &request) {
    if (!hub_.engine().submit(request)) return false;
    // Once submitted, the engine owns the request until done(): never return
    // early. The engine bounds the wait (next poll / action timeout), so the
    // cap is only a sanity check for an engine bug, and fails loudly.
    for (uint32_t waited = 0; !request.done(); waited++) {
        if (waited > kSanityCapMs) {
            std::fprintf(stderr, "hub-sim: engine request not done after %u simulated ms; aborting\n",
                         static_cast<unsigned>(kSanityCapMs));
            std::abort();
        }
        hub_.advance(1);
    }
    return true;
}

api::RuntimeSnapshot SimRuntime::snapshot() const {
    api::RuntimeSnapshot s;
    Engine &engine = hub_.engine();
    s.uptimeMs = hub_.nowMs();
    if (engine.hasActiveActivity()) s.activeActivityId = engine.activeActivityId();
    s.ip = "127.0.0.1";
    s.remote.paired = true;
    s.remote.held = hub_.input().held();
    s.remote.badChecksums = hub_.input().badChecksumCount();
    s.remote.unknownCodes = hub_.input().unknownCodeCount();
    s.diagnostics.queueOverflows = engine.executor().stats().rejected;
    return s;
}

const char *SimRuntime::integrationState(std::string_view integrationId) const {
    const IntegrationSpec *integration = hub_.configService().findIntegration(integrationId);
    if (integration == nullptr) return "not_configured";
    if (!integration->enabled) return "disabled";
    bool up = true;
    if (integration->type == kBleHidType) up = hub_.hid().connected();
    if (integration->type == kHomeAssistantType) up = hub_.homeAssistant().available();
    return up ? "connected" : "disconnected";
}

}  // namespace hubbo::sim
