#include "engine_runtime.h"

#include <Arduino.h>
#include <esp_timer.h>

#include "ble_hid.h"
#include "hubbo/domain/config.h"
#include "console.h"

namespace hubbo::fw {

namespace {

bool isBle(std::string_view integrationId) {
    return integrationId == hubbo::kBleIntegrationId;
}

struct BleStateText {
    const char *state;   // api/openapi.yaml IntegrationState
    const char *detail;  // the peer address is appended when non-null
    bool withPeer;
};

const BleStateText &bleText(uint8_t state) {
    static const BleStateText kText[] = {
        {"error", "Bluetooth failed to start", false},   // Off
        {"not_configured", "No host paired", false},     // NotPaired
        {"connecting", "Pairing window open", false},    // Pairing
        {"disconnected", "Waiting for ", true},          // Waiting
        {"connected", "Connected to ", true},            // Connected
    };
    return kText[state < sizeof(kText) / sizeof(kText[0]) ? state : 0];
}

}  // namespace

void EngineRuntime::publish(const RuntimeStatus &status) {
    portENTER_CRITICAL(&lock_);
    status_ = status;
    portEXIT_CRITICAL(&lock_);
}

RuntimeStatus EngineRuntime::read() const {
    portENTER_CRITICAL(&lock_);
    const RuntimeStatus copy = status_;
    portEXIT_CRITICAL(&lock_);
    return copy;
}

bool EngineRuntime::run(hubbo::EngineRequest &request) {
    if (!engine_.submit(request)) {
        return false;
    }
    const uint32_t startedMs = millis();
    bool reported = false;
    while (!request.done()) {
        vTaskDelay(1);
        if (!reported && millis() - startedMs >= kSlowRequestMs) {
            reported = true;
            con.println("runtime: engine request still running after 2 s");
        }
    }
    return true;
}

hubbo::api::RuntimeSnapshot EngineRuntime::snapshot() const {
    const RuntimeStatus s = read();
    const uint32_t nowMs = millis();

    hubbo::api::RuntimeSnapshot out;
    out.uptimeMs = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    out.activeActivityId = engine_.activeActivityIdShared();
    out.linkUp = s.linkUp;
    out.ip = s.ip;

    out.remote.paired = s.remotePaired;
    out.remote.channel = s.channel;
    out.remote.lastPacketAgeMs = s.lastPacketAtMs == 0 ? -1 : static_cast<int64_t>(nowMs - s.lastPacketAtMs);
    out.remote.held = s.held;
    out.remote.hasLastEvent = s.hasLastEvent;
    out.remote.lastEvent = s.lastEvent;
    out.remote.lastEventAgeMs = s.hasLastEvent ? nowMs - s.lastEvent.atMs : 0;
    out.remote.badChecksums = s.badChecksums;
    out.remote.unknownCodes = s.unknownCodes;

    // Heap figures are thread-safe to read from any task.
    out.diagnostics.heapFree = ESP.getFreeHeap();
    out.diagnostics.heapMinFree = ESP.getMinFreeHeap();
    out.diagnostics.psramFree = ESP.getFreePsram();
    out.diagnostics.queueOverflows = s.queueOverflows;
    out.diagnostics.latencyCount = s.latencyCount;
    out.diagnostics.p50Ms = s.p50Ms;
    out.diagnostics.p95Ms = s.p95Ms;
    out.diagnostics.p99Ms = s.p99Ms;
    return out;
}

uint8_t EngineRuntime::bleState() const {
    portENTER_CRITICAL(&lock_);
    const uint8_t state = status_.bleState;
    portEXIT_CRITICAL(&lock_);
    return state;
}

const char *EngineRuntime::integrationState(std::string_view integrationId) const {
    return isBle(integrationId) ? bleText(bleState()).state : "not_configured";
}

std::string EngineRuntime::integrationDetail(std::string_view integrationId) const {
    if (!isBle(integrationId)) {
        return {};
    }
    const RuntimeStatus s = read();
    const BleStateText &text = bleText(s.bleState);
    return text.withPeer ? std::string(text.detail) + s.blePeer : std::string(text.detail);
}

const char *EngineRuntime::deviceAvailability(const hubbo::DeviceSpec &device) const {
    if (!isBle(device.integrationId)) {
        return "unknown";
    }
    return static_cast<BleHid::State>(bleState()) == BleHid::State::Connected ? "available" : "unavailable";
}

}  // namespace hubbo::fw
