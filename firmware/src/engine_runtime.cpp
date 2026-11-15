#include "engine_runtime.h"

#include <Arduino.h>
#include <esp_timer.h>

#include "hubbo/domain/config.h"
#include "hubbo/integrations/ha_adapter.h"
#include "console.h"

namespace hubbo::fw {

namespace {

bool isBle(std::string_view integrationId) {
    return integrationId == hubbo::kBleIntegrationId;
}

bool isHomeAssistant(std::string_view integrationId) {
    return integrationId == hubbo::api::kHaIntegrationId;
}

bool isMqtt(std::string_view integrationId) {
    return integrationId == hubbo::kMqttIntegrationId;
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
    out.selectedSlot = engine_.selectedSlotShared();
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

const char *EngineRuntime::integrationState(std::string_view integrationId) const {
    const hubbo::IntegrationSpec *integration = config_ != nullptr ? config_->findIntegration(integrationId) : nullptr;
    if (integration == nullptr) {
        return "not_configured";
    }
    if (isHomeAssistant(integrationId)) {
        return hubbo::integrations::haIntegrationState(homeAssistant_, *integration);
    }
    if (isBle(integrationId)) {
        return hubbo::integrations::bleRadioState(*integration, bleRadioReady());
    }
    if (isMqtt(integrationId) && mqtt_ != nullptr) {
        return mqtt_->state();
    }
    return "not_configured";
}

std::string EngineRuntime::integrationDetail(std::string_view integrationId) const {
    if (isMqtt(integrationId)) {
        return mqtt_ != nullptr ? mqtt_->detail() : std::string();
    }
    if (!isBle(integrationId)) {
        return {};
    }
    return hubbo::integrations::bleRadioDetail(bleRadioReady());
}

const char *EngineRuntime::deviceAvailability(const hubbo::DeviceSpec &device) const {
    if (device.provider.type == hubbo::kHomeAssistantType) {
        return hubbo::integrations::haDeviceAvailability(homeAssistant_, device);
    }
    if (!isBle(device.integrationId) || bleHosts_ == nullptr) {
        return "unknown";
    }
    return hubbo::integrations::bleDeviceAvailability(bleHosts_->status(device.id));
}

}  // namespace hubbo::fw
