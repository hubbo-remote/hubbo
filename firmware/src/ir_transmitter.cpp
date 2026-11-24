#include "ir_transmitter.h"

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "board.h"
#include "console.h"

namespace hubbo::fw {

using hubbo::integrations::IrSignal;

namespace {

constexpr int kCore = 0;
// Above lwIP (18) and W5500 RX (15): under HTTP load they hold core 0 for
// over 5 ms at a time, which made a hold's next frame late at priority 6.
// The task runs about 50 us per frame, so it costs them nothing.
constexpr unsigned kPriority = 20;
constexpr uint32_t kStackBytes = 4096;
constexpr uint32_t kMaxHalfTicks = 32767;  // an RMT symbol half's 15-bit duration

}  // namespace

bool RmtIrTransmitter::begin() {
    symbols_ = static_cast<rmt_symbol_word_t *>(
        heap_caps_malloc(kMaxSymbols * sizeof(rmt_symbol_word_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (symbols_ == nullptr) {
        return false;
    }
    // PSRAM stack: the task never touches flash.
    return xTaskCreatePinnedToCoreWithCaps(&RmtIrTransmitter::run, "ir", kStackBytes, this, kPriority, &task_, kCore,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

bool RmtIrTransmitter::send(const IrSignal &signal, uint16_t frames, bool hold) {
    bool idle = false;
    if (!ready() || frames == 0 || !busy_.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
        return false;
    }
    pending_ = signal;
    pendingFrames_ = frames;
    pendingHold_ = hold;
    stopRequested_.store(false, std::memory_order_relaxed);  // only a stop() after this send counts
    failed_.store(false, std::memory_order_relaxed);         // a failure is this send's
    hasPending_.store(true, std::memory_order_release);
    xTaskNotifyGive(task_);
    return true;
}

void RmtIrTransmitter::stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (ready()) {
        xTaskNotifyGive(task_);
    }
}

void RmtIrTransmitter::run(void *self) {
    auto *t = static_cast<RmtIrTransmitter *>(self);
    if (!t->createChannel()) {
        con.println("ir: RMT channel failed; IR is unavailable");
        vTaskDelete(nullptr);
        return;
    }
    t->ready_.store(true, std::memory_order_release);
    con.printf("ir: ready on GPIO %d\n", board::ir::kPin);
    t->loop();
}

bool RmtIrTransmitter::createChannel() {
    rmt_tx_channel_config_t config = {};
    config.gpio_num = static_cast<gpio_num_t>(board::ir::kPin);
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = kResolutionHz;
    config.mem_block_symbols = kRamSymbols;
    config.trans_queue_depth = 2;  // one frame in flight, one queued behind it
    config.flags.init_level = 0;
    esp_err_t err = rmt_new_tx_channel(&config, &channel_);
    if (err == ESP_OK) {
        rmt_copy_encoder_config_t encoderConfig = {};
        err = rmt_new_copy_encoder(&encoderConfig, &encoder_);
    }
    if (err == ESP_OK) {
        rmt_tx_event_callbacks_t callbacks = {};
        callbacks.on_trans_done = &RmtIrTransmitter::onDone;
        err = rmt_tx_register_event_callbacks(channel_, &callbacks, this);
    }
    if (err == ESP_OK) {
        err = rmt_enable(channel_);
    }
    if (err != ESP_OK) {
        con.printf("ir: RMT setup: %s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool IRAM_ATTR RmtIrTransmitter::onDone(rmt_channel_handle_t, const rmt_tx_done_event_data_t *, void *self) {
    auto *t = static_cast<RmtIrTransmitter *>(self);
    t->done_.fetch_add(1, std::memory_order_release);
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(t->task_, &woken);
    return woken == pdTRUE;
}

void RmtIrTransmitter::loop() {
    TickType_t wait = portMAX_DELAY;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, wait);
        wait = portMAX_DELAY;
        if (!active_ && hasPending_.load(std::memory_order_acquire)) {
            startPending();
        }
        if (!active_) {
            continue;
        }
        if (stopRequested_.exchange(false, std::memory_order_acquire)) {
            holding_ = false;
        }
        const uint32_t done = done_.load(std::memory_order_acquire);
        const uint32_t inFlight = queued_ - done;
        if (more() && (inFlight == 0 || esp_timer_get_time() >= inFlightEndUs_ - kQueueLeadUs)) {
            const Section &section = queued_ == 0 ? first_ : repeat_;
            const int64_t from = inFlight == 0 ? esp_timer_get_time() : inFlightEndUs_;
            if (queueFrame(section)) {
                inFlightEndUs_ = from + section.ticks;
                queued_++;
                if (framesLeft_ > 0) {
                    framesLeft_--;
                }
            } else {  // end the send, as failed, once what's in flight is done
                framesLeft_ = 0;
                holding_ = false;
                failed_.store(true, std::memory_order_relaxed);  // published by busy_'s release below
            }
        }
        if (!more() && queued_ == done) {
            active_ = false;
            busy_.store(false, std::memory_order_release);
        } else if (more() && queued_ - done == 1) {
            // Wake when the next frame is due (rounded up: never spin); a
            // done interrupt or a stop() wakes it sooner.
            const int64_t left = inFlightEndUs_ - kQueueLeadUs - esp_timer_get_time();
            wait = left > 0 ? pdMS_TO_TICKS((left + 999) / 1000) : 1;
        }
    }
}

void RmtIrTransmitter::fail() {
    failed_.store(true, std::memory_order_relaxed);
    busy_.store(false, std::memory_order_release);
}

void RmtIrTransmitter::startPending() {
    // busy_ keeps send() off the mailbox until this send ends.
    hasPending_.store(false, std::memory_order_relaxed);
    const IrSignal &signal = pending_;

    if (!build(signal)) {
        con.printf("ir: %s %s not sent: a zero duration, an odd count or over %u symbols\n", signal.deviceId,
                   signal.command, static_cast<unsigned>(kMaxSymbols));
        fail();
        return;
    }
    if (signal.carrierHz != carrierHz_) {
        rmt_carrier_config_t carrier = {};
        carrier.frequency_hz = signal.carrierHz;
        carrier.duty_cycle = kDuty;
        const esp_err_t err = rmt_apply_carrier(channel_, &carrier);
        if (err != ESP_OK) {
            con.printf("ir: %s %s not sent: carrier %lu Hz refused (%s)\n", signal.deviceId, signal.command,
                       static_cast<unsigned long>(signal.carrierHz), esp_err_to_name(err));
            carrierHz_ = 0;
            fail();
            return;
        }
        carrierHz_ = signal.carrierHz;
    }
    active_ = true;
    holding_ = pendingHold_;
    framesLeft_ = pendingFrames_;
    queued_ = 0;
    done_.store(0, std::memory_order_relaxed);  // nothing in flight
}

bool RmtIrTransmitter::build(const IrSignal &signal) {
    size_t used = 0;  // symbols_ filled
    if (signal.carrierHz == 0 || !addSection(signal.first, signal.firstCount, &used, &first_)) {
        return false;
    }
    if (signal.repeat == signal.first && signal.repeatCount == signal.firstCount) {  // one section: built once
        repeat_ = first_;
        return true;
    }
    return addSection(signal.repeat, signal.repeatCount, &used, &repeat_);
}

// Marks are level 1 (the carrier on), spaces level 0. A duration over a
// half's 15 bits is split into equal halves of the same level. An odd number
// of halves splits the section's last one (a space: the gap) in two.
bool RmtIrTransmitter::addSection(const uint32_t *durations, size_t count, size_t *used, Section *out) {
    if (durations == nullptr || count < 2 || count % 2 != 0) {
        return false;
    }
    out->first = static_cast<uint16_t>(*used);
    out->ticks = 0;
    bool havePending = false;
    uint32_t pendingLevel = 0;
    uint32_t pendingTicks = 0;
    auto emit = [&](uint32_t level, uint32_t ticks) {
        if (!havePending) {
            havePending = true;
            pendingLevel = level;
            pendingTicks = ticks;
            return true;
        }
        if (*used >= kMaxSymbols) {
            return false;
        }
        rmt_symbol_word_t &s = symbols_[(*used)++];
        s.level0 = pendingLevel;
        s.duration0 = pendingTicks;
        s.level1 = level;
        s.duration1 = ticks;
        havePending = false;
        return true;
    };
    for (size_t i = 0; i < count; i++) {
        const uint32_t level = i % 2 == 0 ? 1 : 0;
        const uint32_t ticks = durations[i];  // 1 us ticks
        if (ticks == 0) {
            return false;
        }
        out->ticks += ticks;
        uint32_t left = ticks;
        for (uint32_t pieces = (ticks + kMaxHalfTicks - 1) / kMaxHalfTicks; pieces > 0; pieces--) {
            const uint32_t piece = left / pieces;
            if (!emit(level, piece)) {
                return false;
            }
            left -= piece;
        }
    }
    if (havePending) {
        const uint32_t ticks = pendingTicks;
        havePending = false;
        if (ticks < 2 || !emit(0, ticks / 2) || !emit(0, ticks - ticks / 2)) {
            return false;
        }
    }
    out->count = static_cast<uint16_t>(*used - out->first);
    return true;
}

bool RmtIrTransmitter::queueFrame(const Section &section) {
    rmt_transmit_config_t config = {};
    config.flags.eot_level = 0;
    const esp_err_t err =
        rmt_transmit(channel_, encoder_, symbols_ + section.first, section.count * sizeof(rmt_symbol_word_t), &config);
    if (err != ESP_OK) {
        con.printf("ir: rmt_transmit: %s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

}  // namespace hubbo::fw
