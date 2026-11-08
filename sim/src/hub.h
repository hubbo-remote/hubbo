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

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "fakes.h"
#include "hubbo/domain/config.h"
#include "hubbo/engine/compiler.h"
#include "hubbo/engine/engine.h"
#include "hubbo/remote/input.h"

namespace hubbo::sim {

struct LogLine {
    uint32_t atMs;
    std::string text;
};

class SimHub : public EngineObserver {
public:
    SimHub();

    // Compile and publish. On failure nothing changes and the report says why.
    ValidationReport load(const ConfigSpec &config);
    const ConfigSpec &config() const { return config_; }

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
    FakeHomeAssistant &homeAssistant() { return fakes_.homeAssistant; }
    RecordingHidTransport &hid() { return fakes_.hid; }
    integrations::BleHidAdapter &ble() { return fakes_.ble; }
    remote::RemoteInput &input() { return input_; }

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

private:
    void sendGroup(uint8_t group);
    void logCommand(std::string text);
    void logEvent(std::string text);

    // Declaration order is construction order: the engine reads the store
    // and adapters, the input feeds the engine.
    FakeAdapters fakes_;
    Engine::Store store_;
    Engine engine_{store_, fakes_.set};
    remote::RemoteInput input_{engine_};

    ConfigSpec config_;
    uint32_t nowMs_ = 0;
    uint32_t lastPacketMs_ = 0;
    remote::ButtonSet held_ = 0;
    std::vector<LogLine> commands_;
    std::vector<LogLine> events_;
};

}  // namespace hubbo::sim
