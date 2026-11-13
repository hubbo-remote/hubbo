#pragma once

// api::Runtime for the firmware: the bridge between the HTTP task and the
// RF/engine task.
//
//   publish()   engine task (loop), a few times a second: copies status into
//               a fixed-size record under a spinlock. No allocation. (The
//               active activity comes straight from the engine instead.)
//   snapshot()  HTTP task: copies that record out and builds the API struct.
//   run()       HTTP task: Engine::submit(), then vTaskDelay-polls done().
//
// run() returns false only when the request couldn't be submitted (mailbox
// full -> 503). Once submitted it waits for done() however long it takes:
// the request lives on the caller's stack and the engine writes its reply
// there, so returning early would leave the engine a dangling pointer. The
// engine bounds the wait itself (transitions on the next poll, executions by
// the action timeout), and a hung engine task trips the task watchdog.
//
// submit() has one producer: run() is called only from the httpd task.

#include <freertos/FreeRTOS.h>

#include <cstdint>

#include "hubbo/api/api.h"
#include "hubbo/config/service.h"
#include "hubbo/engine/engine.h"
#include "hubbo/integrations/ble_hosts.h"
#include "hubbo/integrations/ha_client.h"
#include "hubbo/integrations/mqtt_bridge.h"
#include "hubbo/remote/input.h"

namespace hubbo::fw {

// What the engine task publishes. Plain data only.
struct RuntimeStatus {
    bool linkUp = false;
    char ip[16] = {};

    bool remotePaired = false;
    int channel = -1;
    uint32_t lastPacketAtMs = 0;  // 0 = never
    hubbo::remote::ButtonSet held = 0;
    bool hasLastEvent = false;
    hubbo::remote::RemoteEvent lastEvent{};
    uint32_t badChecksums = 0;
    uint32_t unknownCodes = 0;

    uint32_t queueOverflows = 0;
    uint32_t latencyCount = 0;  // read -> HID samples
    float p50Ms = 0, p95Ms = 0, p99Ms = 0;
};

class EngineRuntime : public hubbo::api::Runtime {
public:
    EngineRuntime(hubbo::Engine &engine, const hubbo::AdapterSet &adapters,
                  const hubbo::integrations::HomeAssistantClient &homeAssistant)
        : engine_(engine), adapters_(adapters), homeAssistant_(homeAssistant) {}

    // Once at boot, before the API starts: integrationState() looks up the
    // integration's spec (on httpd, the ConfigService's task).
    void setConfig(const hubbo::config::ConfigService *config) { config_ = config; }
    // Once at boot: the `ble` integration's and Bluetooth devices' state
    // (BleHostPort reads are httpd-safe).
    void setBleHosts(const hubbo::integrations::BleHostPort *bleHosts) { bleHosts_ = bleHosts; }
    // Once at boot: the `mqtt_main` integration's state (any-task reads).
    void setMqtt(const hubbo::integrations::MqttBridge *mqtt) { mqtt_ = mqtt; }

    // Engine task.
    void publish(const RuntimeStatus &status);

    // HTTP task.
    bool run(hubbo::EngineRequest &request) override;
    hubbo::api::RuntimeSnapshot snapshot() const override;
    const hubbo::AdapterSet &adapters() const override { return adapters_; }
    const char *integrationState(std::string_view integrationId) const override;
    std::string integrationDetail(std::string_view integrationId) const override;
    const char *deviceAvailability(const hubbo::DeviceSpec &device) const override;

    // Any task: the address as of the last publish() ("" when not up).
    std::string ip() const { return read().ip; }

    static constexpr uint32_t kSlowRequestMs = 2000;  // logged, not abandoned

private:
    RuntimeStatus read() const;
    bool bleRadioReady() const { return bleHosts_ != nullptr && bleHosts_->radioReady(); }

    hubbo::Engine &engine_;
    const hubbo::AdapterSet &adapters_;
    const hubbo::integrations::HomeAssistantClient &homeAssistant_;  // state(): any task
    const hubbo::config::ConfigService *config_ = nullptr;
    const hubbo::integrations::BleHostPort *bleHosts_ = nullptr;
    const hubbo::integrations::MqttBridge *mqtt_ = nullptr;
    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    RuntimeStatus status_;
};

}  // namespace hubbo::fw
