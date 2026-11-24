#include "diagnostics.h"

#include <Arduino.h>

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <freertos/task.h>
#include <freertos/FreeRTOS.h>
#include <atomic>
#include <new>
#include <memory>

#include "hubbo/engine/spsc.h"
#include "console.h"

namespace hubbo::fw::diag {

namespace {

struct LogLine {
    char text[112];
};
hubbo::SpscRing<LogLine, 65> logRing;

struct LoggedPayload {
    uint8_t bytes[32];
    uint8_t length;
    uint8_t pipe;
};
hubbo::SpscRing<LoggedPayload, 17> payloadRing;
uint32_t dropped = 0;
bool raw = false;

LatencyStats eventLatency;
LatencyStats hidLatency;
LatencyStats::Summary hidCached;  // hidSummary()'s, for hidCachedCount samples
uint32_t hidCachedCount = UINT32_MAX;
GapCounters bootGaps;    // since boot (`lat`, and kept across a restart)
GapCounters windowGaps;  // the /debug/stats window
LatencyStats *windowHid = nullptr;  // the window's read->hid (PSRAM, from resetLatency() at boot)
std::atomic<uint32_t> bleSendFailures{0};
// closeWindow(): httpd bumps closeRequested; the loop fills `closed`, then
// sets closedSerial to match. The loop writes `closed` only for a new request,
// so httpd can read it once closedSerial says it's there.
std::atomic<uint32_t> closeRequested{0};
std::atomic<uint32_t> closedSerial{0};
LoopStats closed;

// Survives esp_restart() (not a power cycle). Valid only with the magic.
struct KeptLoopStats {
    uint32_t magic;
    uint32_t maxGapUs;
    uint32_t slowLoops;
    char why[24];
};
constexpr uint32_t kKeptMagic = 0x4C4F4F50;  // "LOOP"
RTC_NOINIT_ATTR KeptLoopStats kept;

// What each core was running when the task watchdog fired: written from its
// interrupt, just before the panic, and printed at the next boot (the panic's
// own output goes to USB, which a PoE board doesn't have).
struct WatchdogRecord {
    uint32_t magic;
    char running[2][configMAX_TASK_NAME_LEN];
};
constexpr uint32_t kWatchdogMagic = 0x54574454;  // "TWDT"
RTC_NOINIT_ATTR WatchdogRecord watchdogRecord;

void printLoopLine(const char *prefix, uint32_t maxGapUs, uint32_t slow, const char *suffix) {
    con.printf("%sloop: max gap %lu us, %lu iterations over 5 ms%s\n", prefix, static_cast<unsigned long>(maxGapUs),
               static_cast<unsigned long>(slow), suffix);
}
uint32_t lastLoopUs = 0;
#ifdef HUBBO_REMOTE_LOG
hubbo::remote::RemoteLog *remoteLog = nullptr;
uint32_t lastStallMs = 0;  // the last RF stall logged
uint32_t lastStallGapUs = 0;
#endif
bool inPayload = false;
uint32_t payloadReadUs = 0;

void printLine(const char *label, const LatencyStats &stats) {
    const LatencyStats::Summary s = stats.summarize();
    if (s.count == 0) {
        con.printf("latency %s: no samples\n", label);
        return;
    }
    con.printf("latency %s: n=%lu (last %lu) p50=%lu us p95=%lu us p99=%lu us max=%lu us\n", label,
                  static_cast<unsigned long>(s.count), static_cast<unsigned long>(s.kept),
                  static_cast<unsigned long>(s.p50), static_cast<unsigned long>(s.p95),
                  static_cast<unsigned long>(s.p99), static_cast<unsigned long>(s.max));
}

}  // namespace

void logf(const char *format, ...) {
    LogLine line;
    va_list args;
    va_start(args, format);
    vsnprintf(line.text, sizeof(line.text), format, args);
    va_end(args);
    if (!logRing.push(line)) {
        dropped++;
    }
}

void setRawLog(bool on) {
    raw = on;
}

bool rawLog() {
    return raw;
}

void logPayload(const uint8_t *bytes, uint8_t length, uint8_t pipe) {
    LoggedPayload entry{};
    memcpy(entry.bytes, bytes, length < sizeof(entry.bytes) ? length : sizeof(entry.bytes));
    entry.length = length;
    entry.pipe = pipe;
    if (!payloadRing.push(entry)) {
        dropped++;
    }
}

void printLogs() {
    LogLine line;
    while (logRing.pop(&line)) {
        con.println(line.text);
    }
    LoggedPayload p;
    while (payloadRing.pop(&p)) {
        char text[16 + 3 * sizeof(p.bytes)];
        int n = snprintf(text, sizeof(text), "raw pipe %u:", p.pipe);
        for (uint8_t i = 0; i < p.length; i++) {
            n += snprintf(text + n, sizeof(text) - n, " %02X", p.bytes[i]);
        }
        con.println(text);
    }
    if (dropped != 0) {
        con.printf("log overflow: %lu dropped\n", static_cast<unsigned long>(dropped));
        dropped = 0;
    }
}

void beginPayload(uint32_t readUs) {
    payloadReadUs = readUs;
    inPayload = true;
}

void endPayload() {
    inPayload = false;
}

void eventEmitted(hubbo::remote::EventKind kind) {
    // Down/Up only: Tap rides on the same Up and would double-count it.
    if (inPayload && (kind == hubbo::remote::EventKind::Down || kind == hubbo::remote::EventKind::Up)) {
        eventLatency.record(micros() - payloadReadUs);
    }
}

bool TimedHidTransport::timed(bool sent) {
    if (!sent) {
        bleSendFailures.fetch_add(1, std::memory_order_relaxed);
    } else if (inPayload) {
        const uint32_t us = micros() - payloadReadUs;
        hidLatency.record(us);
        if (windowHid != nullptr) {
            windowHid->record(us);
        }
    }
    return sent;
}

#ifdef HUBBO_REMOTE_LOG
void setRemoteLog(hubbo::remote::RemoteLog *log) {
    remoteLog = log;
}

bool LoggedHidTransport::logged(std::string_view deviceId, uint32_t startUs, bool sent) {
    if (!sent && remoteLog != nullptr) {
        hubbo::remote::LogEntry entry;
        entry.atMs = millis();
        entry.type = hubbo::remote::LogType::Action;
        entry.result = hubbo::ExecResult::failed(hubbo::ErrorCode::AdapterFailed);
        entry.durationMs = (micros() - startUs) / 1000;
        entry.integrationId.assign(hubbo::kBleIntegrationId);
        entry.deviceId.assign(deviceId);
        remoteLog->append(entry);
    }
    return sent;
}
#endif

void loopTick() {
    const uint32_t nowUs = micros();
    const uint32_t gap = nowUs - lastLoopUs;
    lastLoopUs = nowUs;
    bootGaps.add(gap);
    windowGaps.add(gap);
    const uint32_t request = closeRequested.load(std::memory_order_acquire);
    if (request != closedSerial.load(std::memory_order_relaxed)) {  // close the window, this gap included
        closed.window = windowGaps;
        closed.hid = windowHid != nullptr ? windowHid->summarize() : LatencyStats::Summary{};
        closed.bleSendFailures = bleSendFailures.load(std::memory_order_relaxed);
        windowGaps = {};
        if (windowHid != nullptr) {
            windowHid->reset();
        }
        closedSerial.store(request, std::memory_order_release);
    }
    if (gap > 5000) {
#ifdef HUBBO_REMOTE_LOG
        // One entry per burst (a flash write stalls every few ms): the first,
        // then within a second only a longer stall.
        const uint32_t nowMs = millis();
        if (nowMs - lastStallMs >= 1000 || gap > lastStallGapUs) {
            lastStallMs = nowMs;
            lastStallGapUs = gap;
            hubbo::remote::logAnomaly(remoteLog, nowMs, hubbo::remote::Anomaly::RfStall, gap / 1000);
        }
#endif
    }
}

std::vector<TaskShare> TaskShares::sample(bool rebase) {
    std::vector<TaskShare> rows;
    // On the heap (PSRAM), not the caller's stack: `tasks` runs on the loop.
    const UBaseType_t capacity = uxTaskGetNumberOfTasks() + 4;
    std::unique_ptr<TaskStatus_t[]> tasks(new (std::nothrow) TaskStatus_t[capacity]);
    if (!tasks) {
        return rows;
    }
    uint32_t total = 0;
    const UBaseType_t n = uxTaskGetSystemState(tasks.get(), capacity, &total);
    const uint32_t elapsed = total - baseTotal_;  // run-time clock ticks, per core
    rows.resize(n);
    const TaskHandle_t loopTask = xTaskGetHandle("loopTask");
    for (UBaseType_t i = 0; i < n; i++) {
        const TaskStatus_t &t = tasks[i];
        uint32_t base = 0;
        for (const Seen &seen : base_) {
            if (seen.handle == t.xHandle && strcmp(seen.name, t.pcTaskName) == 0) {
                base = seen.runTime;
                break;
            }
        }
        const uint32_t ran = t.ulRunTimeCounter - base;  // modular: the counters wrap (~72 min)
        TaskShare &row = rows[i];
        snprintf(row.name, sizeof(row.name), "%s", t.pcTaskName);
        row.core = t.xCoreID == tskNO_AFFINITY ? -1 : static_cast<int8_t>(t.xCoreID);
        row.prio = static_cast<uint8_t>(t.uxCurrentPriority);
        row.stackFreeMin = t.usStackHighWaterMark;  // bytes on ESP-IDF
        row.per10k = elapsed ? static_cast<uint32_t>((uint64_t{ran} * 10000) / elapsed) : 0;
        row.shareKnown = t.xHandle != loopTask;
    }
    if (rebase) {
        base_.resize(n);
        for (UBaseType_t i = 0; i < n; i++) {
            base_[i].handle = tasks[i].xHandle;
            snprintf(base_[i].name, sizeof(base_[i].name), "%s", tasks[i].pcTaskName);
            base_[i].runTime = tasks[i].ulRunTimeCounter;
        }
        baseTotal_ = total;
    }
    return rows;
}

void printTasks() {
    static TaskShares *shares = new (std::nothrow) TaskShares;  // in PSRAM, made after setup()
    const std::vector<TaskShare> rows = shares != nullptr ? shares->sample(true) : std::vector<TaskShare>();
    if (rows.empty()) {
        con.println("tasks: no memory");
        return;
    }
    static bool first = true;
    con.printf("tasks: %u, %s\n", static_cast<unsigned>(rows.size()),
               first ? "first call: totals since boot" : "share of one core since the last `tasks`");
    first = false;
    for (const TaskShare &t : rows) {
        char share[12] = "      -";  // loopTask: unknown (diagnostics.h)
        if (t.shareKnown) {
            snprintf(share, sizeof(share), "%3lu.%02lu%%", static_cast<unsigned long>(t.per10k / 100),
                     static_cast<unsigned long>(t.per10k % 100));
        }
        con.printf("  %-16s core %-3s prio %2u  %s\n", t.name, t.core < 0 ? "any" : t.core == 0 ? "0" : "1",
                   static_cast<unsigned>(t.prio), share);
    }
}

void printLatency() {
    printLine("read->event", eventLatency);
    printLine("read->hid", hidLatency);
    printLoopLine("", bootGaps.maxUs, bootGaps.over5ms, " (since boot)");
}

void keepLoopStatsAcrossRestart(const char *why) {
    // Plain reads of loopTask's counters: at worst one sample stale.
    const uint32_t gapUs = bootGaps.maxUs;
    const uint32_t slow = bootGaps.over5ms;
    printLoopLine("", gapUs, slow, " (since boot)");
    KeptLoopStats k = {kKeptMagic, gapUs, slow, {}};
    snprintf(k.why, sizeof(k.why), "%s", why != nullptr ? why : "?");
    kept = k;
}

}  // namespace hubbo::fw::diag

extern "C" void esp_task_wdt_isr_user_handler(void) {
    using hubbo::fw::diag::watchdogRecord;
    for (BaseType_t core = 0; core < 2; core++) {
        const TaskHandle_t task = xTaskGetCurrentTaskHandleForCore(core);
        const char *name = task != nullptr ? pcTaskGetName(task) : "?";
        size_t i = 0;
        for (; name != nullptr && name[i] != '\0' && i + 1 < sizeof(watchdogRecord.running[core]); i++) {
            watchdogRecord.running[core][i] = name[i];
        }
        watchdogRecord.running[core][i] = '\0';
    }
    watchdogRecord.magic = hubbo::fw::diag::kWatchdogMagic;
}

namespace hubbo::fw::diag {

void printPreviousRun() {
    const bool recorded = watchdogRecord.magic == kWatchdogMagic;
    watchdogRecord.magic = 0;  // each boot: a record is about the reset just before it
    if (recorded && esp_reset_reason() == ESP_RST_TASK_WDT) {
        con.printf("previous run: task watchdog fired with core 0 running %s, core 1 running %s\n",
                   watchdogRecord.running[0], watchdogRecord.running[1]);
    }
    KeptLoopStats k = kept;
    kept.magic = 0;
    if (k.magic != kKeptMagic) {
        return;  // a power-on or a crash
    }
    k.why[sizeof(k.why) - 1] = '\0';
    char prefix[sizeof(k.why) + 32];
    snprintf(prefix, sizeof(prefix), "previous run (restart: %s): ", k.why);
    printLoopLine(prefix, k.maxGapUs, k.slowLoops, "");
}

void resetLatency() {
    eventLatency.reset();
    hidLatency.reset();
    hidCachedCount = UINT32_MAX;
    if (windowHid == nullptr) {  // the first call, at the end of setup()
        if (void *mem = heap_caps_malloc(sizeof(LatencyStats), MALLOC_CAP_SPIRAM)) {
            windowHid = new (mem) LatencyStats();
        }
    }
    lastLoopUs = micros();
}

bool closeWindow(LoopStats *out) {
    static uint32_t answered = 0;  // httpd only: the last close handed out
    if (closeRequested.load(std::memory_order_relaxed) == answered) {
        closeRequested.store(answered + 1, std::memory_order_release);
    }  // else a close the loop hasn't answered yet: wait for that one
    const uint32_t request = closeRequested.load(std::memory_order_relaxed);
    for (int waitedMs = 0; waitedMs < 200; waitedMs++) {
        if (closedSerial.load(std::memory_order_acquire) == request) {
            *out = closed;
            answered = request;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

LatencyStats::Summary hidSummary() {
    if (hidLatency.count() != hidCachedCount) {
        hidCached = hidLatency.summarize();
        hidCachedCount = hidLatency.count();
    }
    return hidCached;
}

}  // namespace hubbo::fw::diag
