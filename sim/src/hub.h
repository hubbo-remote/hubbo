#pragma once

// SimHub: the real core (RemoteInput -> Engine -> Executor -> adapters)
// wired to simulated adapters and a deterministic millisecond clock.
// Button presses are turned into real radio payloads (full held set per
// group, keepalives every 40 ms while held), so the simulator exercises the
// same input decoding as the firmware.
//
// Everything that happens is appended to two logs:
//   commands  one line per dispatched action:
//             "<integration> <device|-> <command> [k=v ...] -> <status>"
//   events    activity changes, sequence outcomes, unbound triggers, ...
//
// Home Assistant: the hub always has the real HomeAssistantClient over an
// in-process FakeHaServer (the API's settings and discovery use it). With
// `realHomeAssistant`, the engine's "ha_main" is the real
// HomeAssistantAdapter over that client (snapshot -> buildServiceCall ->
// call_service -> result), as on the board; hub-sim serve runs this way.
// Otherwise it's the scripted FakeHomeAssistant (scenarios, engine tests:
// immediate results, injectable latency and failures).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fake_ha.h"
#include "fake_mqtt.h"
#include "fakes.h"
#include "release_feed.h"
#include "hubbo/config/service.h"
#include "hubbo/config/boot.h"
#include "hubbo/engine/active_persister.h"
#include "hubbo/domain/config.h"
#include "hubbo/engine/compiler.h"
#include "hubbo/api/factory_reset_port.h"
#include "hubbo/api/firmware_update_port.h"
#include "hubbo/api/remote_port.h"
#include "hubbo/api/restart_port.h"
#include "hubbo/config/hub.h"
#include "hubbo/engine/engine.h"
#include "hubbo/integrations/ha_adapter.h"
#include "hubbo/integrations/ha_client.h"
#include "hubbo/integrations/mqtt_bridge.h"
#include "hubbo/remote/input.h"
#include "hubbo/remote/log.h"

namespace hubbo::sim {

// hub-sim's MAC, for its default name: "Hubbo 534D" (hostname hubbo-534d),
// fixed so tests stay stable. Locally administered.
inline constexpr uint8_t kSimMac[6] = {0x02, 0x00, 0x00, 0x00, 0x53, 0x4D};

struct LogLine {
    uint32_t atMs;
    std::string text;
};

class SimHub : public EngineObserver, private remote::EventSink {
public:
    // Config and the active activity persist as records ("config",
    // "active_activity") in `storage` (e.g. a StdioStorage --data dir), or
    // in memory when null; `state` overrides where the active activity goes
    // (tests). `bonds` (optional) keeps the Bluetooth bonds across restarts,
    // as the board's NVS does: apart from the config's medium, so its health
    // doesn't touch them. Without one, or on a first start, the fixture
    // world's bond. Nothing is loaded yet: call boot() or load().
    explicit SimHub(storage::Storage *storage = nullptr, storage::StateStore *state = nullptr,
                    bool realHomeAssistant = false, storage::Storage *bonds = nullptr);
    static constexpr const char *kBondsFile = "ble_bonds";

    // Simulated power-on through bootHub() (config/boot.h), the same path
    // as the firmware: load the config record (else `defaults`), the
    // credentials (if given), then restore the saved active activity without
    // running its startup.
    config::LoadReport boot(const ConfigSpec &defaults, auth::Credentials *credentials = nullptr);
    const BootReport &lastBoot() const { return boot_; }
    // Graceful stop: write anything pending (the debounced active activity).
    void flush() {
        persister_.flush(engine_.activeActivityIdShared(), service_.state() == config::ConfigState::Ok);
    }
    // Saved active activity -> none (factory reset).
    void forgetActive() { state_.set(ActivePersister::kKey, ""); }

    // Restore the whole config through the ConfigService (validated, saved,
    // published). On failure nothing changes and the report says why.
    ValidationReport load(const ConfigSpec &config);
    const ConfigSpec &config() const { return service_.config(); }

    // Factory reset in place: fixtures-style config replaces everything,
    // nothing is active, fake faults are cleared (the fake HA back to its
    // entities, the client reconnected), the remote
    // radio is unpaired, Bluetooth is back to kBondedDevice bonded to
    // kBondedHost and connected (the radio working), no button stays
    // held.
    // Persisted like any edit. References to the hub stay valid.
    ValidationReport factoryReset(const ConfigSpec &config);
    // The hub side of a real factory reset (POST /admin/factory-reset), as a
    // wiped board's restart: no key held, nothing active, the remote endpoint
    // and every Bluetooth bond forgotten, the config record gone, then boot() with
    // the firmware's defaults (an empty config plus the built-ins), saved as
    // generation 1. Erase the secrets first: boot() reloads `credentials`.
    void wipe(auth::Credentials *credentials);
    config::ConfigService &configService() { return service_; }
    storage::Storage &storage() { return *storage_; }
    storage::RecordStore &records() { return records_; }

    // Remote. press/release change the held set and send its payload now.
    void press(remote::Button button);
    void release(remote::Button button);
    void tap(remote::Button button, uint32_t holdMs = 100);  // press, advance, release
    // Let time pass in 1 ms steps; keepalives while held unless `silent`
    // (a lost release: RemoteInput times out after 500 ms).
    void advance(uint32_t ms, bool silent = false);

    ErrorCode activate(const std::string &id) { return engine_.activate(id, nowMs_); }
    ErrorCode off() { return engine_.off(nowMs_); }

    uint32_t nowMs() const { return nowMs_; }
    Engine &engine() { return engine_; }
    Engine::Store &store() { return store_; }
    AdapterSet &adapters() { return fakes_.set; }
    // The scripted ha_main (unused with realHomeAssistant).
    FakeHomeAssistant &homeAssistant() { return fakes_.homeAssistant; }
    bool realHomeAssistant() const { return realHomeAssistant_; }
    FakeHaServer &haServer() { return ha_.server; }
    integrations::HomeAssistantClient &haClient() { return ha_.client; }
    FakeHaTransport &haTransport() { return ha_.transport; }
    // The remote radio (the API's RemotePort); its scan clock is hub time.
    api::SimulatedRemote &remote() { return remote_; }
    // The Bluetooth radio and its hosts (the API's BleHostPort and the BLE
    // adapter's transport); the pairing window runs on hub time.
    SimulatedBleHosts &bleHosts() { return fakes_.bleHosts; }
    // The fixture world's bond, at start and after a reset: the Apple TV.
    static constexpr const char *kBondedDevice = "dev_apple_tv";
    static constexpr const char *kBondedHost = "Apple TV";
    // Firmware updates (the API's FirmwareUpdatePort); writes and the
    // simulated reboot run on hub time. A reset gives a fresh one.
    api::SimulatedFirmwareUpdate &firmware() { return firmware_; }
    // Online updates (the API's FirmwareReleasePort) over the fake release
    // feed (/sim/release); checks and downloads run on hub time. A reset
    // clears the feed and what was checked.
    SimFirmwareReleases &releases() { return releases_; }
    // Called when a simulated reboot into a new image finishes (the board
    // restarts: SimApi drops every session).
    std::function<void()> onFirmwareRebooted;
    // The API's FactoryResetPort: the restart comes kRestartMs of hub time
    // after the request and calls onFactoryReset (SimApi: erase everything).
    // /sim/reset gives a fresh port (ready, nothing pending).
    api::SimulatedFactoryReset &factoryResetPort() { return factoryReset_; }
    config::HubSettings &hubSettings() { return hubSettings_; }
    // Hubbo as an HA device over MQTT (the API's ApiContext::mqtt), against
    // the fake broker. Its node id comes from kSimMac. It runs on hub time
    // in serviceIntegrations(); its settings come from api::applyMqttSettings.
    integrations::MqttBridge &mqtt() { return mqtt_; }
    FakeMqttBroker &mqttBroker() { return mqttBroker_; }
    // How the bridge's activity commands reach the engine: SimApi sets it to
    // api::runTransition over its Runtime ("" = off). The bridge only posts
    // one; it runs after the current advance() step, as httpd would run it.
    std::function<void(const std::string &activityId)> runTransition;
    api::SimulatedRestart &restartPort() { return restart_; }
    std::function<void()> onFactoryReset;
    // One turn of the integration task (transport, client, adapter) at the
    // current time, repeated while the client has work (discovery parse
    // steps). advance() runs it every ms.
    void serviceIntegrations();
    integrations::BleHidAdapter &ble() { return fakes_.ble; }
    remote::RemoteInput &input() { return input_; }
    // The remote log (remote/log.h; the API's GET /remote/log), filled from
    // the simulated presses. Cleared by a reset or wipe, as a restart would.
    remote::RemoteLog *remoteLog() { return remoteLog_.get(); }
    // The signal flag every simulated packet carries (strong until set;
    // /sim/remote's ?signal=). A reset sets it back to strong.
    void setSignal(remote::Signal signal) { signal_ = signal; }
    // What the radio saw, as the board reports it in /remote/status: when
    // the last payload arrived (-1: never) and the last remote event.
    int64_t lastPacketAgeMs() const { return anyPacket_ ? static_cast<int64_t>(nowMs_ - lastPacketMs_) : -1; }
    const remote::RemoteEvent *lastEvent() const { return hasLastEvent_ ? &lastEvent_ : nullptr; }

    std::vector<LogLine> &commands() { return commands_; }
    std::vector<LogLine> &events() { return events_; }

    // Called for every log line as it happens (e.g. to print a live trace).
    std::function<void(const char *kind, const LogLine &)> onLog;

    // EngineObserver
    void onActionDispatched(const DispatchRecord &record) override;
    void onActionCompleted(const CompletionRecord &record) override;
    void onSequenceFinished(const SequenceReport &report) override;
    void onActivityChanged(const char *activityId, uint32_t nowMs) override;
    void onTransitionRejected(ErrorCode error, const char *activityId, uint32_t nowMs) override;
    void onConfigChanged(uint32_t revision, uint32_t nowMs) override;
    void onUnbound(const remote::RemoteEvent &event) override;
    // remote::EventSink, as the firmware's EventTap: remember the event,
    // then hand it to the engine.
    void onRemoteEvent(const remote::RemoteEvent &event) override;

private:
    void sendGroup(uint8_t group);
    void resetBle();  // fresh Bluetooth hosts: kBondedDevice bonded to kBondedHost, connected
    void stopEverything();  // keys released, nothing active or saved active, remote unpaired
    api::SimulatedFactoryReset freshFactoryResetPort();  // erase calls onFactoryReset
    void advanceFirmware(uint32_t ms);
    integrations::MqttBridge::Hooks mqttHooks();
    void logCommand(std::string text);
    void logEvent(std::string text);

    // Declaration order is construction order: the engine reads the store
    // and adapters, the input feeds the engine.
    struct RealHa {
        FakeHaServer server;
        FakeHaTransport transport{server};
        integrations::HomeAssistantClient client{transport};
        integrations::HomeAssistantAdapter adapter{"ha_main", client};
    };
    bool realHomeAssistant_;
    RealHa ha_;
    api::SimulatedRemote remote_;
    api::SimulatedFirmwareUpdate firmware_;
    SimFirmwareReleases releases_{firmware_, [this] { return nowMs_; }};
    api::SimulatedFactoryReset factoryReset_;
    FakeAdapters fakes_;
    integrations::BleHostReconciler bleReconciler_{fakes_.bleHosts};  // hosts follow the saved config
    Engine::Store store_;
    RunHistory runs_;  // the board's lives in PSRAM; here a member will do
    remote::MutexLogLock logLock_;
    std::unique_ptr<remote::RemoteLog> remoteLog_ = std::make_unique<remote::RemoteLog>(logLock_);  // ~900 KB
    remote::Signal signal_ = remote::Signal::Strong;
    Engine engine_{store_, fakes_.set};
    remote::RemoteInput input_{*this};  // events go through onRemoteEvent() to the engine
    storage::MemoryStorage memory_;
    storage::Storage *storage_;
    storage::SlotRecordStore records_;
    config::ConfigService service_;
    storage::RecordStateStore recordState_;
    storage::StateStore &state_;
    ActivePersister persister_;
    config::HubSettings hubSettings_{state_, config::defaultHubName(kSimMac)};
    api::SimulatedRestart restart_;
    FakeMqttBroker mqttBroker_;
    FakeMqttTransport mqttTransport_{mqttBroker_};
    integrations::MqttBridge mqtt_;
    integrations::MqttActivitySync mqttActivities_{mqtt_};
    config::ConfigFanOut configListeners_{&bleReconciler_, &mqttActivities_};  // as the firmware wires them
    uint32_t activityChanges_ = 1;  // the firmware's activityChanges counter
    std::vector<std::string> mqttTransitions_;  // posted by the bridge, run by advance()
    bool runningTransition_ = false;
    BootReport boot_;

    uint32_t nowMs_ = 0;
    uint32_t lastPacketMs_ = 0;
    bool anyPacket_ = false;
    bool hasLastEvent_ = false;
    remote::RemoteEvent lastEvent_{};
    remote::ButtonSet held_ = 0;
    std::vector<LogLine> commands_;
    std::vector<LogLine> events_;
};

}  // namespace hubbo::sim
