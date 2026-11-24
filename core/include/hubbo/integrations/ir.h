#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "hubbo/domain/adapter.h"
#include "hubbo/domain/ids.h"
#include "hubbo/domain/limits.h"
#include "hubbo/integrations/ir_transmitter.h"

// The IR integration (docs/design/ir.md): every device is a profile with
// `platform: ir`, sent through the hub's one emitter (IrTransmitter).
//
// Commands for a device, all in the `remote` group, as Bluetooth's:
//   press {command, hold_ms?}   a tap, or with hold_ms (the step's, else the
//                               command's) a hold for that long
//   press_down / press_up {command}   a hold until the release
//
// What's sent follows the profile spec's IR section:
//   - a tap: the once section, then repeat frames up to timing.ir_repeats
//     frames in all (0 and 1: one frame);
//   - a hold: what a tap sends, then repeat frames until the release (or
//     its hold_ms). A lost release is ended by the remote's release timeout
//     (an up event like any other), and releaseAll() ends a binding's hold;
//   - toggle codes: one bit per device (RAM, starts at 0), flipped on each
//     new press of a command with pronto_toggle; the matching code is sent;
//   - timing.key_delay_ms: from the end of one command to the start of the
//     next to the same device;
//   - one code at a time: a command waits for the one in flight to end, and
//     any new command (another device's, or another on the same device)
//     ends a running hold; a frame in flight always finishes.
//
// Results: a tap and a timed hold are Pending until the code has been sent
// (so a sequence's next step never overlaps it), Accepted at once for a
// direct execute (ExecRequest::answerAtStart). press_down and press_up are
// Accepted at once. Up to kQueueSize commands wait their turn; more is
// QueueFull. Not ready (no emitter): AdapterUnavailable.
namespace hubbo::integrations {

enum class IrCommand : uint8_t { Press, PressDown, PressUp };

// One Pronto code laid out in microseconds: carrier, and where its once
// and repeat sections (counts of durations) sit in a durations vector.
struct IrCodeLayout {
    uint32_t carrierHz = 0;
    uint32_t at = 0, once = 0, repeat = 0;
};
// Appends the code's durations (words as pronto::parse() appended them,
// header first) to `durations` and says where they went.
IrCodeLayout appendIrCode(const uint16_t *words, std::vector<uint32_t> *durations);
// What the emitter sends for it (the spec's frames: the first is the once
// section, or the repeat section if that's empty; every later one the
// repeat section, or the once section). Points into `durations`, which
// must outlive the transmission. The adapter and the firmware's console
// both build signals this way.
IrSignal irSignal(const IrCodeLayout &code, const std::vector<uint32_t> &durations, const char *deviceId = "",
                  const char *command = "");

// The `ir` integration's state for /status: "disabled", "ready" or
// "not_ready" (the emitter isn't set up).
const char *irIntegrationState(const IntegrationSpec &integration, bool transmitterReady);
// Its detail line: "" when ready, else a short reason.
std::string irIntegrationDetail(bool transmitterReady);

class IrAdapter : public IntegrationAdapter {
public:
    static constexpr size_t kQueueSize = 8;

    IrAdapter(const char *integrationId, IrTransmitter &transmitter);
    ~IrAdapter() override;

    const char *integrationId() const override { return id_; }
    const char *integrationType() const override;
    // The device's catalog (IrCommand order). Its codes are parsed into
    // microseconds here, once per distinct profile (commands and timing),
    // shared by every device and compile with it. It lives while anything
    // holds its CommandCatalog::owner (compiled configs, executor work,
    // held keys, this adapter's jobs): a profile no one uses is freed,
    // possibly on the engine task when the last work for it ends. Empty
    // without a profile. Config task.
    CommandCatalog commandsFor(const DeviceSpec &device) const override;
    ExecResult execute(const ExecRequest &request) override;
    void releaseAll(uint32_t nowMs) override;
    void cancel(Ticket ticket, uint32_t nowMs) override;
    void poll(uint32_t nowMs, CompletionSink &sink) override;

    // A restart: nothing queued or running (a running code isn't stopped;
    // its ticket is forgotten), every toggle bit back to 0. Engine task.
    void reset();

    // For tests and the simulator.
    size_t queued() const { return queued_; }
    bool sending() const { return running_.inUse(); }
    bool toggle(std::string_view deviceId) const;

private:
    struct DeviceCatalog;

    enum class Kind : uint8_t { Tap, Timed, Hold };
    struct Job {
        IdString deviceId;  // empty: free
        std::shared_ptr<const DeviceCatalog> catalog;  // null: free
        uint16_t command = 0;
        Kind kind = Kind::Tap;
        Ticket ticket = 0;
        bool reports = false;   // completes its ticket when sent (a Pending tap or timed hold)
        bool released = false;  // a hold whose release came (or a timed hold's time ran out)
        uint32_t holdMs = 0;    // Timed
        uint32_t startedMs = 0;
        bool stopped = false;   // stop() sent
        bool refused = false;   // the emitter refused it: poll() fails it
        bool toggled = false;   // it flipped its device's toggle bit
        bool inUse() const { return catalog != nullptr; }
    };
    struct DeviceState {
        IdString deviceId;  // empty: free
        bool toggle = false;
        uint32_t endedMs = 0;  // when its last command ended
    };

    void start(uint32_t nowMs);  // the next job, if it may go (nothing running)
    void endHoldIfWaited();      // stop a running hold that has a command waiting, or was released
    DeviceState &state(std::string_view deviceId, uint32_t nowMs);
    void removeQueued(size_t index);

    const char *id_;
    IrTransmitter &transmitter_;
    Job queue_[kQueueSize];
    size_t queued_ = 0;
    Job running_;
    DeviceState devices_[limits::kMaxDevices];
    mutable std::mutex catalogsMutex_;
    mutable std::vector<std::weak_ptr<const DeviceCatalog>> catalogs_;  // to share; expired ones are swept
};

}  // namespace hubbo::integrations
