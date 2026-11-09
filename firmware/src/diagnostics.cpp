#include "diagnostics.h"

#include <Arduino.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <freertos/task.h>
#include <freertos/FreeRTOS.h>
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
uint32_t maxLoopGapUs = 0;
uint32_t slowLoops = 0;  // iterations over 5 ms
uint32_t lastLoopUs = 0;
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
    if (sent && inPayload) {
        hidLatency.record(micros() - payloadReadUs);
    }
    return sent;
}

void loopTick() {
    const uint32_t nowUs = micros();
    const uint32_t gap = nowUs - lastLoopUs;
    lastLoopUs = nowUs;
    if (gap > maxLoopGapUs) {
        maxLoopGapUs = gap;
    }
    if (gap > 5000) {
        slowLoops++;
    }
}

void printTasks() {
    // Matched on handle and name: a deleted task's TCB can be reused.
    struct Seen {
        TaskHandle_t handle;
        char name[configMAX_TASK_NAME_LEN];
        uint32_t runTime;
    };
    static Seen previous[48];
    static size_t previousCount = 0;
    static uint32_t previousTotal = 0;

    const UBaseType_t count = uxTaskGetNumberOfTasks();
    std::unique_ptr<TaskStatus_t[]> tasks(new (std::nothrow) TaskStatus_t[count + 4]);
    if (!tasks) {
        con.println("tasks: no memory");
        return;
    }
    uint32_t total = 0;
    const UBaseType_t n = uxTaskGetSystemState(tasks.get(), count + 4, &total);
    const uint32_t elapsed = total - previousTotal;  // run-time clock ticks, per core
    con.printf("tasks: %u, %s\n", static_cast<unsigned>(n),
               previousCount ? "share of one core since the last `tasks`" : "first call: totals since boot");
    for (UBaseType_t i = 0; i < n; i++) {
        const TaskStatus_t &t = tasks[i];
        uint32_t base = 0;
        for (size_t j = 0; j < previousCount; j++) {
            if (previous[j].handle == t.xHandle && strcmp(previous[j].name, t.pcTaskName) == 0) {
                base = previous[j].runTime;
                break;
            }
        }
        const uint32_t ran = t.ulRunTimeCounter >= base ? t.ulRunTimeCounter - base : 0;
        const uint32_t per10k = elapsed ? static_cast<uint32_t>((uint64_t{ran} * 10000) / elapsed) : 0;
        char core[4];
        snprintf(core, sizeof(core), "%s", t.xCoreID == tskNO_AFFINITY ? "any" : t.xCoreID == 0 ? "0" : "1");
        con.printf("  %-16s core %-3s prio %2u  %3lu.%02lu%%\n", t.pcTaskName, core,
                   static_cast<unsigned>(t.uxCurrentPriority), static_cast<unsigned long>(per10k / 100),
                   static_cast<unsigned long>(per10k % 100));
    }
    previousCount = n < 48 ? n : 48;
    for (size_t j = 0; j < previousCount; j++) {
        previous[j].handle = tasks[j].xHandle;
        snprintf(previous[j].name, sizeof(previous[j].name), "%s", tasks[j].pcTaskName);
        previous[j].runTime = tasks[j].ulRunTimeCounter;
    }
    previousTotal = total;
}

void printLatency() {
    printLine("read->event", eventLatency);
    printLine("read->hid", hidLatency);
    con.printf("loop: max gap %lu us, %lu iterations over 5 ms\n", static_cast<unsigned long>(maxLoopGapUs),
                  static_cast<unsigned long>(slowLoops));
}

void resetLatency() {
    eventLatency.reset();
    hidLatency.reset();
    maxLoopGapUs = 0;
    slowLoops = 0;
    lastLoopUs = micros();
}

LatencyStats::Summary hidSummary() {
    static LatencyStats::Summary cached;
    static uint32_t cachedCount = UINT32_MAX;
    if (hidLatency.count() != cachedCount) {
        cached = hidLatency.summarize();
        cachedCount = hidLatency.count();
    }
    return cached;
}

}  // namespace hubbo::fw::diag
