#include "hubbo/remote/log.h"

#include <iterator>
#include <mutex>

#include "hubbo/engine/executor.h"

namespace hubbo::remote {

namespace {

template <typename Enum, size_t N>
const char *nameOf(const char *const (&names)[N], Enum value) {
    const auto i = static_cast<size_t>(value);
    return i < N ? names[i] : "?";
}

constexpr const char *kLogTypeNames[] = {"packet", "event", "action", "anomaly"};
constexpr const char *kPacketKindNames[] = {"down", "hold", "repeat", "up"};
constexpr const char *kSignalNames[] = {"unknown", "strong", "weak"};
constexpr const char *kAnomalyNames[] = {"bad_checksum", "unknown_code", "rf_stall"};
constexpr const char *kBindingKindNames[] = {"command", "macro", "activity"};
static_assert(std::size(kLogTypeNames) == static_cast<size_t>(LogType::Anomaly) + 1, "one name per LogType");
static_assert(std::size(kPacketKindNames) == static_cast<size_t>(PacketKind::Up) + 1, "one name per PacketKind");
static_assert(std::size(kSignalNames) == static_cast<size_t>(Signal::Weak) + 1, "one name per Signal");
static_assert(std::size(kAnomalyNames) == static_cast<size_t>(Anomaly::RfStall) + 1, "one name per Anomaly");
static_assert(std::size(kBindingKindNames) == static_cast<size_t>(BindingKind::Activity) + 1,
              "one name per BindingKind");

}  // namespace

const char *logTypeName(LogType type) { return nameOf(kLogTypeNames, type); }
const char *packetKindName(PacketKind kind) { return nameOf(kPacketKindNames, kind); }
const char *signalName(Signal signal) { return nameOf(kSignalNames, signal); }
bool signalFromName(std::string_view name, Signal *out) {
    for (size_t i = 0; i < std::size(kSignalNames); ++i) {
        if (name == kSignalNames[i]) {
            *out = static_cast<Signal>(i);
            return true;
        }
    }
    return false;
}
const char *anomalyName(Anomaly anomaly) { return nameOf(kAnomalyNames, anomaly); }
const char *bindingKindName(BindingKind binding) { return nameOf(kBindingKindNames, binding); }

#ifdef HUBBO_REMOTE_LOG

namespace {

// The lowest button in `set`, or Button::Count.
Button lowest(ButtonSet set) {
    return set == 0 ? Button::Count : static_cast<Button>(__builtin_ctzll(set));
}

void logAction(RemoteLog *log, const SequenceInfo &sequence, const char *integrationId, const char *deviceId,
               const CommandSpec *command, ExecResult result, uint32_t atMs, uint32_t startedMs) {
    LogEntry entry;
    entry.atMs = atMs;
    entry.type = LogType::Action;
    switch (sequence.origin) {
        case SequenceOrigin::Binding:
        case SequenceOrigin::GlobalBinding:
        case SequenceOrigin::SmartButton:
            entry.hasTrigger = true;
            break;
        case SequenceOrigin::Macro:
            entry.hasTrigger = true;
            entry.binding = BindingKind::Macro;
            break;
        case SequenceOrigin::Startup:
        case SequenceOrigin::Shutdown:
            entry.binding = BindingKind::Activity;
            break;
        case SequenceOrigin::Api:
            break;
    }
    entry.trigger = sequence.trigger;
    entry.result = result;
    entry.durationMs = atMs - startedMs;
    entry.integrationId.assign(integrationId != nullptr ? integrationId : "");
    entry.deviceId.assign(deviceId != nullptr ? deviceId : "");
    if (command != nullptr) entry.commandId = command->id;
    log->append(entry);
}

}  // namespace

void RemoteLog::append(const LogEntry &entry) {
    std::lock_guard<LogLock> locked(lock_);
    LogEntry &slot = entries_[written_ % kRemoteLogCapacity];
    if (written_ >= kRemoteLogCapacity && entry.atMs - slot.atMs < kRemoteLogWindowMs) {
        dropped_++;  // pushed out while still in the window
        lastDroppedMs_ = slot.atMs;
    }
    slot = entry;
    written_++;
}

size_t RemoteLog::copyFrom(uint64_t *next, LogEntry *out, size_t max) const {
    std::lock_guard<LogLock> locked(lock_);
    const uint64_t oldest = written_ > kRemoteLogCapacity ? written_ - kRemoteLogCapacity : 0;
    if (*next < oldest) *next = oldest;  // pushed out meanwhile
    size_t n = 0;
    for (; n < max && *next < written_; ++n, ++*next) out[n] = entries_[*next % kRemoteLogCapacity];
    return n;
}

RemoteLogInfo RemoteLog::info(uint32_t nowMs) const {
    std::lock_guard<LogLock> locked(lock_);
    uint32_t span = nowMs - sinceMs_;
    if (span > kRemoteLogWindowMs) span = kRemoteLogWindowMs;
    if (dropped_ != 0 && nowMs - lastDroppedMs_ < span) span = nowMs - lastDroppedMs_;
    return {span, dropped_};
}

void RemoteLog::clear(uint32_t nowMs) {
    std::lock_guard<LogLock> locked(lock_);
    written_ = 0;
    dropped_ = 0;
    sinceMs_ = nowMs;
}

void logPayload(RemoteLog *log, uint32_t atMs, DecodeResult decoded, const DecodedPacket &packet, ButtonSet before,
                Signal signal) {
    if (log == nullptr) return;
    if (decoded == DecodeResult::BadChecksum) {
        logAnomaly(log, atMs, Anomaly::BadChecksum);
        return;
    }
    const ButtonSet after =
        decoded == DecodeResult::Buttons ? (before & ~groupMask(packet.group)) | packet.held : before;
    LogEntry entry;
    entry.atMs = atMs;
    entry.type = LogType::Packet;
    entry.signal = signal;
    if (decoded == DecodeResult::NotButtons) {
        entry.packet = PacketKind::Hold;
        entry.button = lowest(after);
    } else if (const ButtonSet pressed = after & ~before) {
        entry.packet = PacketKind::Down;
        entry.button = lowest(pressed);
    } else if (const ButtonSet released = before & ~after) {
        entry.packet = PacketKind::Up;
        entry.button = lowest(released);
    } else {
        const ButtonSet group = after & groupMask(packet.group);
        entry.packet = group != 0 ? PacketKind::Repeat : PacketKind::Up;  // an all-clear that released nothing
        entry.button = lowest(group);
    }
    log->append(entry);
    if (packet.unknownCodes > 0) logAnomaly(log, atMs, Anomaly::UnknownCode);
}

void logEvent(RemoteLog *log, const RemoteEvent &event) {
    if (log == nullptr) return;
    LogEntry entry;
    entry.atMs = event.atMs;
    entry.type = LogType::Event;
    entry.button = event.button;
    entry.event = event.kind;
    entry.timedOut = event.timedOut;
    log->append(entry);
}

void logAnomaly(RemoteLog *log, uint32_t atMs, Anomaly anomaly, uint32_t stallMs) {
    if (log == nullptr) return;
    LogEntry entry;
    entry.atMs = atMs;
    entry.type = LogType::Anomaly;
    entry.anomaly = anomaly;
    entry.stallMs = stallMs;
    log->append(entry);
}

void logDispatched(RemoteLog *log, const DispatchRecord &record) {
    if (log == nullptr || record.result.status == ExecStatus::Pending) return;  // logged when it completes
    logAction(log, record.sequence, record.integrationId, record.target, record.action.command, record.result,
              record.atMs, record.atMs);
}

void logCompleted(RemoteLog *log, const CompletionRecord &record) {
    if (log == nullptr) return;
    logAction(log, record.sequence, record.integrationId, record.target, record.command, record.result, record.atMs,
              record.startedMs);
}

#endif

}  // namespace hubbo::remote
