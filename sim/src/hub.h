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
#include <string>
#include <vector>

#include "fake_ha.h"
#include "fakes.h"
#include "hubbo/config/service.h"
#include "hubbo/config/boot.h"
#include "hubbo/engine/active_persister.h"
#include "hubbo/domain/config.h"
#include "hubbo/engine/compiler.h"
#include "hubbo/api/ble_pairing_port.h"
#include "hubbo/api/remote_port.h"
#include "hubbo/engine/engine.h"
#include "hubbo/integrations/ha_adapter.h"
#include "hubbo/integrations/ha_client.h"
#include "hubbo/remote/input.h"

namespace hubbo::sim {

struct LogLine {
    uint32_t atMs;
    std::string text;
};

class SimHub : public EngineObserver, private remote::EventSink {
public:
    // Config and the active activity persist as records ("config",
    // "active_activity") in `storage` (e.g. a StdioStorage --data dir), or
    // in memory when null; `state` overrides where the active activity goes
    // (tests). Nothing is loaded yet: call boot() or load().
    explicit SimHub(storage::Storage *storage = nullptr, storage::StateStore *state = nullptr,
                    bool realHomeAssistant = false);

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
    // entities, the client reconnected), the BLE host reconnects, the remote
    // radio is unpaired, BLE is bonded to kBondedHost again, no button stays
    // held.
    // Persisted like any edit. References to the hub stay valid.
    ValidationReport factoryReset(const ConfigSpec &config);
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
    api::SimulatedBlePairing &blePairing() { return blePairing_; }
    static constexpr const char *kBondedHost = "Apple TV";  // BLE's bond at start and after a reset
    // One turn of the integration task (transport, client, adapter) at the
    // current time, repeated while the client has work (discovery parse
    // steps). advance() runs it every ms.
    void serviceIntegrations();
    RecordingHidTransport &hid() { return fakes_.hid; }
    integrations::BleHidAdapter &ble() { return fakes_.ble; }
    remote::RemoteInput &input() { return input_; }
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
    void resetBle();  // a fresh pairing port, bonded to kBondedHost
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
    api::SimulatedBlePairing blePairing_;
    FakeAdapters fakes_;
    Engine::Store store_;
    Engine engine_{store_, fakes_.set};
    remote::RemoteInput input_{*this};  // events go through onRemoteEvent() to the engine
    storage::MemoryStorage memory_;
    storage::Storage *storage_;
    storage::SlotRecordStore records_;
    config::ConfigService service_;
    storage::RecordStateStore recordState_;
    storage::StateStore &state_;
    ActivePersister persister_;
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
