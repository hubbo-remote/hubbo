#include "diagnostics.h"

#include <Arduino.h>

#include <esp_attr.h>

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

// Survives esp_restart() (not a power cycle). Valid only with the magic.
struct KeptLoopStats {
    uint32_t magic;
    uint32_t maxGapUs;
    uint32_t slowLoops;
    char why[24];
};
constexpr uint32_t kKeptMagic = 0x4C4F4F50;  // "LOOP"
RTC_NOINIT_ATTR KeptLoopStats kept;

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
    if (sent && inPayload) {
        hidLatency.record(micros() - payloadReadUs);
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
    if (gap > maxLoopGapUs) {
        maxLoopGapUs = gap;
    }
    if (gap > 5000) {
        slowLoops++;
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
    printLoopLine("", maxLoopGapUs, slowLoops, "");
}

void keepLoopStatsAcrossRestart(const char *why) {
    // Plain reads of loopTask's counters: at worst one sample stale.
    const uint32_t gapUs = maxLoopGapUs;
    const uint32_t slow = slowLoops;
    printLoopLine("", gapUs, slow, " (since boot or `lat reset`)");
    KeptLoopStats k = {kKeptMagic, gapUs, slow, {}};
    snprintf(k.why, sizeof(k.why), "%s", why != nullptr ? why : "?");
    kept = k;
}

void printPreviousRun() {
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
