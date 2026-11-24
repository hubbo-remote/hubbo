#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "hubbo/domain/adapter.h"
#include "hubbo/domain/ids.h"
#include "hubbo/domain/trigger.h"
#include "hubbo/remote/button.h"
#include "hubbo/remote/input.h"
#include "hubbo/remote/packet.h"

#ifdef HUBBO_REMOTE_LOG
#include <mutex>
#endif

// The remote log (docs/design/remote-log.md): the last 5 minutes of what the
// remote sent and what each press did, in RAM only, for GET /remote/log.
//
// Dev builds only: it exists when HUBBO_REMOTE_LOG is defined (firmware sets
// it for builds that aren't from the release workflow; hub-sim and the host
// tests always do). Without it RemoteLog is an empty type nobody creates, the
// hooks below are empty inlines, and no memory is allocated: whoever holds a
// `RemoteLog *` holds null, and the API reads null as "no log".
//
// Writers: the RF loop (packets, events, anomalies, through RemoteInput), the
// engine task (action results) and the firmware's own tasks (a Bluetooth send
// result, an RF stall). An append is a short critical section behind
// LogLock (the board: a portMUX spinlock; the host: std::mutex) copying one
// entry into a preallocated ring: nothing allocates or waits. Readers copy a
// few entries per lock, so a read never holds the lock for long.
//
// Bounds: entries older than kRemoteLogWindowMs are never read; the count
// bound (kRemoteLogCapacity) pushes out the oldest first. A pushed-out entry
// younger than the window counts as `dropped` and shortens `spanMs`.
namespace hubbo {
struct DispatchRecord;
struct CompletionRecord;
}  // namespace hubbo

namespace hubbo::remote {

constexpr uint32_t kRemoteLogWindowMs = 5 * 60 * 1000;
// A held button sends a keepalive ("hold") packet about every 40 ms: 25 a
// second, 7500 in 5 minutes, plus its events and actions.
constexpr size_t kRemoteLogCapacity = 8192;

enum class LogType : uint8_t { Packet, Event, Action, Anomaly };
// Packet: Down (a button payload pressing a button), Up (one releasing a
// button, or the group's all-clear), Repeat (a button payload re-sending the
// same held set), Hold (the keepalive sent while a button is held).
enum class PacketKind : uint8_t { Down, Hold, Repeat, Up };
enum class Anomaly : uint8_t { BadChecksum, UnknownCode, RfStall };
// What a press ran: a binding's command(s), a macro, an activity's
// startup or shutdown.
enum class BindingKind : uint8_t { Command, Macro, Activity };

const char *logTypeName(LogType type);              // "packet", "event", "action", "anomaly"
const char *packetKindName(PacketKind kind);        // "down", "hold", "repeat", "up"
const char *signalName(Signal signal);              // "unknown", "strong", "weak"
bool signalFromName(std::string_view name, Signal *out);  // hub-sim's ?signal=
const char *anomalyName(Anomaly anomaly);           // "bad_checksum", "unknown_code", "rf_stall"
const char *bindingKindName(BindingKind binding);   // "command", "macro", "activity"

// One entry. Only the fields of its `type` mean anything.
struct LogEntry {
    uint32_t atMs = 0;  // hub uptime (the engine clock)
    LogType type = LogType::Packet;

    // Packet and Event. Button::Count: none (a keepalive with nothing held,
    // an all-clear that released nothing).
    Button button = Button::Count;
    PacketKind packet = PacketKind::Down;  // Packet
    Signal signal = Signal::Unknown;       // Packet
    EventKind event = EventKind::Down;     // Event
    bool timedOut = false;                 // Event: an Up/Tap the hub made when the up packet never came

    // Action: the step's final result (a Pending one once it's known).
    bool hasTrigger = false;  // false: not a press (an API execute, an activation from the API)
    Trigger trigger;
    BindingKind binding = BindingKind::Command;
    ExecResult result;          // Accepted / Skipped / NoReply are ok; Failed / Timeout carry the error
    uint32_t durationMs = 0;    // dispatch to result (0: answered at once)
    IdString integrationId;     // "" if none
    IdString deviceId;          // "" if none (a system step's activity id for system.activate)
    const char *commandId = ""; // the catalog's (adapter-owned, lives as long as the adapters); "" if none

    // Anomaly.
    Anomaly anomaly = Anomaly::BadChecksum;
    uint32_t stallMs = 0;  // RfStall only
};

// The platform's lock: short critical sections, any task.
class LogLock {
public:
    virtual ~LogLock() = default;
    virtual void lock() = 0;
    virtual void unlock() = 0;
};

struct RemoteLogInfo {
    uint32_t spanMs = 0;   // how far back the log is complete
    uint32_t dropped = 0;  // pushed out by the count bound while younger than the window (since boot)
};

#ifdef HUBBO_REMOTE_LOG

// The host's lock (hub-sim, tests).
class MutexLogLock : public LogLock {
public:
    void lock() override { mutex_.lock(); }
    void unlock() override { mutex_.unlock(); }

private:
    std::mutex mutex_;
};

// About 110 bytes an entry, so about 900 KB: the board puts it in PSRAM
// (heap-allocate it, never on a stack).
class RemoteLog {
public:
    explicit RemoteLog(LogLock &lock) : lock_(lock) {}
    RemoteLog(const RemoteLog &) = delete;
    RemoteLog &operator=(const RemoteLog &) = delete;

    // Any task, never blocks for long.
    void append(const LogEntry &entry);

    // Any task: every entry within the window, oldest first, as
    // fn(const LogEntry &, uint32_t ageMs), called outside the lock. An
    // entry pushed out during the read is skipped (and counted in dropped).
    template <typename Fn>
    RemoteLogInfo read(uint32_t nowMs, Fn &&fn) const {
        LogEntry chunk[kReadChunk];
        uint64_t next = 0;
        for (;;) {
            const size_t n = copyFrom(&next, chunk, kReadChunk);
            for (size_t i = 0; i < n; ++i) {
                const auto age = static_cast<int32_t>(nowMs - chunk[i].atMs);  // < 0: stamped just after nowMs
                if (age <= static_cast<int32_t>(kRemoteLogWindowMs)) fn(chunk[i], age < 0 ? 0u : static_cast<uint32_t>(age));
            }
            if (n < kReadChunk) break;
        }
        return info(nowMs);
    }
    RemoteLogInfo info(uint32_t nowMs) const;

    // As after a restart (hub-sim's reset): empty, nothing dropped, and the
    // span starts at `nowMs`.
    void clear(uint32_t nowMs);

private:
    static constexpr size_t kReadChunk = 8;
    // Copies up to `max` entries from sequence number *next on (skipping
    // any pushed out), and moves *next past them. At most `max` copies per
    // lock, whatever the ring holds: read() filters by age outside it.
    size_t copyFrom(uint64_t *next, LogEntry *out, size_t max) const;

    LogLock &lock_;
    LogEntry entries_[kRemoteLogCapacity];
    uint64_t written_ = 0;  // entries ever appended: entry n is at n % capacity
    uint32_t dropped_ = 0;
    uint32_t lastDroppedMs_ = 0;  // the newest dropped entry's time
    uint32_t sinceMs_ = 0;        // clear()'s time (0: boot)
};

// The hooks (the callers pass their log, or null).
// RemoteInput's, per payload, before its events: a packet entry (none for a
// bad checksum) and its anomalies. `before`: every button held before it.
void logPayload(RemoteLog *log, uint32_t atMs, DecodeResult decoded, const DecodedPacket &packet, ButtonSet before,
                Signal signal);
void logEvent(RemoteLog *log, const RemoteEvent &event);
void logAnomaly(RemoteLog *log, uint32_t atMs, Anomaly anomaly, uint32_t stallMs = 0);
// The engine's: a dispatched step's result unless Pending, and a Pending
// step's result when it arrives.
void logDispatched(RemoteLog *log, const DispatchRecord &record);
void logCompleted(RemoteLog *log, const CompletionRecord &record);

#else

// Never created (the pointer is always null); the calls compile so readers
// need no #if of their own.
class RemoteLog {
public:
    void append(const LogEntry &) {}
    template <typename Fn>
    RemoteLogInfo read(uint32_t, Fn &&) const {
        return {};
    }
    RemoteLogInfo info(uint32_t) const { return {}; }
    void clear(uint32_t) {}
};

inline void logPayload(RemoteLog *, uint32_t, DecodeResult, const DecodedPacket &, ButtonSet, Signal) {}
inline void logEvent(RemoteLog *, const RemoteEvent &) {}
inline void logAnomaly(RemoteLog *, uint32_t, Anomaly, uint32_t = 0) {}
inline void logDispatched(RemoteLog *, const DispatchRecord &) {}
inline void logCompleted(RemoteLog *, const CompletionRecord &) {}

#endif

}  // namespace hubbo::remote
