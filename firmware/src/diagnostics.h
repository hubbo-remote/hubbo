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
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>  // TaskShares

#ifdef HUBBO_REMOTE_LOG
#include <Arduino.h>  // micros()
#endif

#include "hubbo/integrations/ble_hid.h"
#include "hubbo/remote/input.h"
#include "hubbo/remote/log.h"
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
#ifdef HUBBO_REMOTE_LOG
// The remote log for the loop's RF stalls and the Bluetooth send failures
// below (dev builds). Set once in setup().
void setRemoteLog(hubbo::remote::RemoteLog *log);
#endif
// Call once per loop iteration. A gap over 5 ms is an RF stall in the remote
// log (dev builds).
void loopTick();
void printLatency();
// Before a deliberate restart (any task): prints the loop line ("loop: max
// gap ...", as printLatency()) and keeps it in RTC memory, which survives a
// software restart. printPreviousRun() logs it again at the next boot, so
// /debug/log shows a reboot's stall (a firmware update's write) afterwards.
void keepLoopStatsAcrossRestart(const char *why);
void printPreviousRun();  // boot, once
// CPU share per task (core affinity, priority) since the previous call.
void printTasks();

// One task's numbers, as TaskShares::sample() reports them.
struct TaskShare {
    char name[configMAX_TASK_NAME_LEN];
    int8_t core;  // 0, 1, or -1: no affinity
    uint8_t prio;
    uint32_t stackFreeMin;  // bytes, since the task started
    uint32_t per10k;        // share of one core since the baseline, x10000
    bool shareKnown;        // false for loopTask (below): per10k means nothing
};
// Each task's CPU share since a baseline (the first: since boot), from
// FreeRTOS's run-time counters. Owned by one caller, which keeps its own
// baseline. A task's counter advances only when it's switched out, and
// loopTask never blocks, so its count is stale: its share is left unknown.
class TaskShares {
public:
    // Every task; empty if out of memory. `rebase`: the next sample counts
    // from now.
    std::vector<TaskShare> sample(bool rebase);

private:
    struct Seen {  // matched on handle and name: a deleted task's TCB can be reused
        TaskHandle_t handle;
        char name[configMAX_TASK_NAME_LEN];
        uint32_t runTime;
    };
    std::vector<Seen> base_;
    uint32_t baseTotal_ = 0;
};

// Starts a new measuring window: read->hid latency and the window's loop
// counters (`lat reset`). The since-boot loop counters keep going.
void resetLatency();
// read->hid summary; sorts only when there are new samples.
LatencyStats::Summary hidSummary();

// --- Board budget stats (GET /debug/stats; README "Board budget")
// The loop's gaps between iterations.
struct GapCounters {
    uint32_t maxUs = 0;
    uint32_t over5ms = 0;
    void add(uint32_t gapUs) {
        maxUs = gapUs > maxUs ? gapUs : maxUs;
        over5ms += gapUs > 5000;
    }
};
// A closed /debug/stats window. Its read->hid is the window's own recorder:
// closing it leaves the since-boot one (`lat`, /api/v1/status) alone.
struct LoopStats {
    GapCounters window;
    LatencyStats::Summary hid;     // read->hid
    uint32_t bleSendFailures = 0;  // since boot
};
// httpd only: the loop closes the window at its next pass and starts the
// next. True with the closed window in `out`. False if the loop didn't
// answer within 200 ms; that close stays pending, and the next call returns
// it, so no window is lost.
bool closeWindow(LoopStats *out);

// HidTransport decorator that records read->hid for reports caused by a
// radio payload (not by poll-driven events such as timed-out releases).
class TimedHidTransport : public hubbo::integrations::HidTransport {
public:
    explicit TimedHidTransport(hubbo::integrations::HidTransport &inner) : inner_(inner) {}
    bool connected(std::string_view deviceId) const override { return inner_.connected(deviceId); }
    bool sendKeyboard(std::string_view deviceId, const uint8_t report[8]) override {
        return timed(inner_.sendKeyboard(deviceId, report));
    }
    bool sendConsumer(std::string_view deviceId, uint16_t usage) override {
        return timed(inner_.sendConsumer(deviceId, usage));
    }

private:
    static bool timed(bool sent);
    hubbo::integrations::HidTransport &inner_;
};

#ifdef HUBBO_REMOTE_LOG
// HidTransport decorator that puts a failed send, and how long it took, in
// the remote log (dev builds). Successes aren't logged: the engine logs the
// steps.
class LoggedHidTransport : public hubbo::integrations::HidTransport {
public:
    explicit LoggedHidTransport(hubbo::integrations::HidTransport &inner) : inner_(inner) {}
    bool connected(std::string_view deviceId) const override { return inner_.connected(deviceId); }
    bool sendKeyboard(std::string_view deviceId, const uint8_t report[8]) override {
        const uint32_t startUs = micros();
        return logged(deviceId, startUs, inner_.sendKeyboard(deviceId, report));
    }
    bool sendConsumer(std::string_view deviceId, uint16_t usage) override {
        const uint32_t startUs = micros();
        return logged(deviceId, startUs, inner_.sendConsumer(deviceId, usage));
    }

private:
    static bool logged(std::string_view deviceId, uint32_t startUs, bool sent);
    hubbo::integrations::HidTransport &inner_;
};
#endif

}  // namespace hubbo::fw::diag
