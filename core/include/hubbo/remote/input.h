#pragma once

#include <cstdint>

#include "hubbo/remote/button.h"
#include "hubbo/remote/packet.h"

// Turns the remote's "here is everything held" packets into per-button
// trigger events. This is the first stage of the hot path: no allocation,
// no I/O, bounded work per call.
//
// Events per button press:
//   Down  - the button went down. Immediate.
//   Hold  - still down after holdMs. Fires once per press.
//   Up    - the button came up (or its release was lost; see below).
//   Tap   - came up before Hold fired. Emitted right after Up.
//
// Bind Down/Up for key-like behaviour (BLE key held exactly as long as the
// button). Bind Tap + Hold for click-or-hold: Tap can only be known at
// release, so it lags by the press duration.
//
// Lost releases: while anything is held the remote sends a packet at least
// every ~40 ms. If no packet of any kind arrives for releaseTimeoutMs, every
// held button is released with `timedOut` set, so nothing stays stuck.
namespace hubbo::remote {

class RemoteLog;  // log.h

enum class EventKind : uint8_t { Down, Up, Tap, Hold, Count };

constexpr size_t kEventKindCount = static_cast<size_t>(EventKind::Count);

const char *eventKindName(EventKind kind);
bool eventKindFromName(const char *name, EventKind *out);

struct RemoteEvent {
    Button button;
    EventKind kind;
    uint32_t atMs;          // when it happened (not when it was reported)
    bool timedOut = false;  // Up/Tap synthesized after a lost release
};

class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void onRemoteEvent(const RemoteEvent &event) = 0;
};

struct InputConfig {
    uint32_t holdMs = 800;           // Harmonizer's BLUETOOTH_HOLD_MS
    uint32_t releaseTimeoutMs = 500; // Harmonizer's BLUETOOTH_KEY_RELEASE_TIMEOUT_MS
};

class RemoteInput {
public:
    RemoteInput(EventSink &sink, InputConfig config = {});

    // Feed every received payload, button or not. Due Hold/timeout events
    // are emitted first, so event order is deterministic for any call order.
    // `signal`: the radio's, for the remote log.
    void onPayload(const uint8_t *bytes, size_t length, uint32_t nowMs, Signal signal = Signal::Unknown);

    // The remote log (log.h): set once, before payloads arrive. Null: none.
    void setLog(RemoteLog *log) { log_ = log; }

    // Call often (every loop iteration) to fire Hold and release timeouts.
    void poll(uint32_t nowMs);

    // Release everything now (e.g. radio stopped). Emits Up (and Tap if due).
    void releaseAll(uint32_t nowMs);

    ButtonSet held() const { return held_; }

    uint32_t badChecksumCount() const { return badChecksums_; }
    uint32_t unknownCodeCount() const { return unknownCodes_; }

private:
    void applyGroup(uint8_t group, ButtonSet now, uint32_t nowMs);
    void press(Button button, uint32_t nowMs);
    void release(Button button, uint32_t nowMs, bool timedOut);
    void releaseHeld(uint32_t atMs, bool timedOut);
    void emit(Button button, EventKind kind, uint32_t atMs, bool timedOut = false);

    EventSink &sink_;
    InputConfig config_;
    RemoteLog *log_ = nullptr;
    ButtonSet held_ = 0;
    ButtonSet holdFired_ = 0;
    uint32_t downAtMs_[kButtonCount] = {};
    uint32_t lastPacketMs_ = 0;
    uint32_t badChecksums_ = 0;
    uint32_t unknownCodes_ = 0;
};

}  // namespace hubbo::remote
