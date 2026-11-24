#pragma once

// The IR transmitter (docs/design/ir.md): core's integrations::IrTransmitter
// port on the RMT TX peripheral, with a hardware carrier, on board::ir::kPin.
//
// Everything RMT happens on the "ir" task (core 0). It creates the channel
// there, so the RMT interrupt is allocated on core 0 too, and the RF loop on
// core 1 is never interrupted by IR. send() and stop() only set a mailbox and
// notify the task, so the engine task (the loop) never blocks on IR.
//
// A frame is one section sent as one RMT transaction. A section of up to
// kRamSymbols - 1 symbols sits whole in the channel's own RAM, so the frame
// needs no interrupt until it ends, and a flash write (which holds off
// non-IRAM interrupts on both cores) can't touch it. A longer one is refilled
// from the interrupt every half of that RAM, tens of milliseconds apart for
// real codes. The next frame is queued kQueueLeadUs before the one in flight
// ends, so frames go back to back, and a stop() until then ends the send
// after the frame in flight.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <driver/rmt_tx.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "hubbo/integrations/ir_transmitter.h"

namespace hubbo::fw {

class RmtIrTransmitter : public hubbo::integrations::IrTransmitter {
public:
    static constexpr uint32_t kResolutionHz = 1000000;  // 1 us ticks; a symbol half holds up to 32,767 us
    static constexpr size_t kRamSymbols = 192;          // all 4 TX memory blocks
    // Both sections (core caps each at 512 pairs), plus long halves split.
    static constexpr size_t kMaxSymbols = 1100;
    static constexpr float kDuty = 0.33f;
    static constexpr int64_t kQueueLeadUs = 5000;

    // Starts the task; the channel is made on it, and ready() turns true once
    // it's up. False if the task couldn't start.
    bool begin();

    // Any task (atomic loads).
    bool ready() const override { return ready_.load(std::memory_order_acquire); }
    bool busy() const override { return busy_.load(std::memory_order_acquire); }
    // Never block.
    bool send(const hubbo::integrations::IrSignal &signal, uint16_t frames, bool hold) override;
    void stop() override;
    bool takeFailed() override { return failed_.exchange(false, std::memory_order_acq_rel); }

private:
    struct Section {
        uint16_t first = 0;  // index into symbols_
        uint16_t count = 0;
        uint32_t ticks = 0;  // its length
    };

    static void run(void *self);
    void loop();
    bool createChannel();
    void startPending();
    void fail();  // ends the send as failed (busy_ drops)
    bool build(const hubbo::integrations::IrSignal &signal);
    bool addSection(const uint32_t *durations, size_t count, size_t *used, Section *out);
    bool more() const { return framesLeft_ > 0 || holding_; }
    bool queueFrame(const Section &section);

    TaskHandle_t task_ = nullptr;
    std::atomic<bool> ready_{false};
    std::atomic<bool> busy_{false};
    std::atomic<bool> failed_{false};  // the last send failed after it was accepted

    // Mailbox: written by whoever set busy_ (send()), read by the ir task
    // after hasPending_.
    hubbo::integrations::IrSignal pending_;
    uint16_t pendingFrames_ = 0;
    bool pendingHold_ = false;
    std::atomic<bool> hasPending_{false};
    std::atomic<bool> stopRequested_{false};

    // Task-owned.
    rmt_channel_handle_t channel_ = nullptr;
    rmt_encoder_handle_t encoder_ = nullptr;
    rmt_symbol_word_t *symbols_ = nullptr;  // PSRAM: the interrupt isn't IRAM-safe, so it never runs with the cache off
    uint32_t carrierHz_ = 0;
    Section first_;
    Section repeat_;
    bool active_ = false;
    bool holding_ = false;
    uint32_t framesLeft_ = 0;    // not yet queued
    uint32_t queued_ = 0;        // frames handed to the driver, this send
    int64_t inFlightEndUs_ = 0;  // when the last queued frame should end

    // Counted by the done interrupt (DRAM: the object is a static global).
    std::atomic<uint32_t> done_{0};

    static bool onDone(rmt_channel_handle_t, const rmt_tx_done_event_data_t *, void *self);
};

}  // namespace hubbo::fw
