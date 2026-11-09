#pragma once

// Serial diagnostics for the RF/engine task: a log ring and the latency
// probe. Everything here runs on loopTask.
//
// Log: the hot path only formats lines into a ring; printLogs() writes them
// to serial after the hot path, so serial I/O never sits between a radio
// read and a HID report.
//
// Latency, from micros() right after a payload leaves the nRF24 FIFO:
//   read->event  to a Down/Up event reaching the engine (decode cost)
//   read->hid    to the HID report being queued to NimBLE (full dispatch)
// plus the longest loop iteration. See README "Latency measurement".

#include <cstddef>
#include <cstdint>

#include "hubbo/integrations/ble_hid.h"
#include "hubbo/remote/input.h"
#include "latency.h"

namespace hubbo::fw::diag {

// --- Log
void logf(const char *format, ...) __attribute__((format(printf, 1, 2)));
// Raw payloads, only while enabled (`raw on`).
void setRawLog(bool on);
bool rawLog();
void logPayload(const uint8_t *bytes, uint8_t length, uint8_t pipe);
void printLogs();

// --- Latency
// Brackets the handling of one radio payload.
void beginPayload(uint32_t readUs);
void endPayload();
// A remote event reached the engine.
void eventEmitted(hubbo::remote::EventKind kind);
// Call once per loop iteration.
void loopTick();
void printLatency();
void resetLatency();
// read->hid summary; sorts only when there are new samples.
LatencyStats::Summary hidSummary();

// HidTransport decorator that records read->hid for reports caused by a
// radio payload (not by poll-driven events such as timed-out releases).
class TimedHidTransport : public hubbo::integrations::HidTransport {
public:
    explicit TimedHidTransport(hubbo::integrations::HidTransport &inner) : inner_(inner) {}
    bool connected() const override { return inner_.connected(); }
    bool sendKeyboard(const uint8_t report[8]) override { return timed(inner_.sendKeyboard(report)); }
    bool sendConsumer(uint16_t usage) override { return timed(inner_.sendConsumer(usage)); }

private:
    static bool timed(bool sent);
    hubbo::integrations::HidTransport &inner_;
};

}  // namespace hubbo::fw::diag
