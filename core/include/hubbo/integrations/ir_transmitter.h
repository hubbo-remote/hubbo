#pragma once

#include <cstdint>

// The IR transmitter port (docs/design/ir.md). One emitter, one code at a
// time, provided by each platform:
//   firmware  the RMT channel on GPIO 1, with the code's carrier;
//   hub-sim   a fake that records what was sent (sim/src/fakes.h).
//
// The core (IrAdapter, integrations/ir.h) decides everything the spec says
// about frames, repeats, holds, toggles and key delay; the port only puts
// marks and spaces on the wire:
//
//   send(signal, frames, hold)
//     frame 1 is signal.first, then signal.repeat until `frames` frames in
//     all (frames >= 1). With hold, it then keeps sending signal.repeat
//     until stop(). Frames go back to back: a section's last duration is
//     the gap (carrier off) after it, and it's part of the frame.
//   stop()
//     A hold ends after its `frames` frames and the frame in flight, or the
//     next one if it's already queued (the board queues each repeat frame a
//     few ms ahead to keep them back to back); never cuts a frame. No effect
//     on a send without hold, or when idle.
//   busy()
//     True from send() until the last frame's gap has passed, or until the
//     transmission failed.
//   takeFailed()
//     Once busy() is false: whether that transmission failed after send()
//     accepted it (the code didn't fit, the carrier was refused, the driver
//     errored), so the device may not have seen it. Reading clears it.
//
// Threading: every method is called on the engine task and must not block:
// send() starts the transmission and returns; the platform finishes it on
// its own path (RMT, its interrupt or task) without the engine task.
// The durations stay valid until busy() is false (the adapter keeps their
// catalog until then); the platform may read them while sending or copy
// them at send().
namespace hubbo::integrations {

// One code ready to send: Pronto parsed into microseconds. Each section is
// marks and spaces alternating, starting with a mark, an even count >= 2.
struct IrSignal {
    uint32_t carrierHz = 0;  // 1,000,000 / (F x 0.241246); any F > 0 (the catalog has 9 kHz codes)
    const uint32_t *first = nullptr;  // the first frame: the once section, or the repeat frame if that's empty
    uint16_t firstCount = 0;
    const uint32_t *repeat = nullptr;  // every later frame: the repeat section, or the once section if that's empty
    uint16_t repeatCount = 0;
    // For logs only (the console, hub-sim's /sim/ir): never null.
    const char *deviceId = "";
    const char *command = "";
};

class IrTransmitter {
public:
    virtual ~IrTransmitter() = default;
    // The emitter is set up and can send. Not ready: IR commands fail with
    // adapter_unavailable and the `ir` integration says so.
    virtual bool ready() const = 0;
    // A transmission is under way (see above).
    virtual bool busy() const = 0;
    // Start one transmission. False (nothing sent) if busy or not ready.
    virtual bool send(const IrSignal &signal, uint16_t frames, bool hold) = 0;
    // End a hold (see above).
    virtual void stop() = 0;
    // The last transmission failed (see above); clears it.
    virtual bool takeFailed() = 0;
};

}  // namespace hubbo::integrations
