#include "hubbo/integrations/ir.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

#include "hubbo/domain/config.h"
#include "hubbo/domain/pronto.h"
#include "hubbo/domain/time.h"
#include "remote_group.h"

namespace hubbo::integrations {

namespace {

// The executor's backstop: waiting behind other codes, the key delay, then
// sending (a hold up to its hold_ms).
constexpr uint32_t kPressTimeoutMs = limits::kMaxHoldMs + limits::kMaxKeyDelayMs + 10000;

// Timing a catalog is built for: frames per tap (ir_repeats, at least one)
// and the key delay.
uint16_t framesOf(const DeviceProfile &p) {
    return static_cast<uint16_t>(std::max<uint32_t>(1, p.timing ? p.timing->irRepeats.value_or(1) : 1));
}
uint32_t keyDelayOf(const DeviceProfile &p) { return p.timing ? p.timing->keyDelayMs.value_or(0) : 0; }

// The same signal as these Pronto words.
bool sameSignal(const IrCodeLayout &code, const uint16_t *words, const std::vector<uint32_t> &durations) {
    const uint16_t f = pronto::frequency(words);
    if (code.carrierHz != pronto::carrierHz(f) || code.once != 2 * pronto::oncePairs(words) ||
        code.repeat != 2 * pronto::repeatPairs(words)) {
        return false;
    }
    for (uint32_t i = 0; i < code.once + code.repeat; i++) {
        if (durations[code.at + i] != pronto::durationUs(words[pronto::kHeaderWords + i], f)) return false;
    }
    return true;
}

}  // namespace

IrCodeLayout appendIrCode(const uint16_t *words, std::vector<uint32_t> *durations) {
    const uint16_t f = pronto::frequency(words);
    IrCodeLayout code;
    code.carrierHz = pronto::carrierHz(f);
    code.at = static_cast<uint32_t>(durations->size());
    code.once = static_cast<uint32_t>(2 * pronto::oncePairs(words));
    code.repeat = static_cast<uint32_t>(2 * pronto::repeatPairs(words));
    for (size_t i = pronto::kHeaderWords; i < pronto::wordCount(words); i++) {
        durations->push_back(pronto::durationUs(words[i], f));
    }
    return code;
}

IrSignal irSignal(const IrCodeLayout &code, const std::vector<uint32_t> &durations, const char *deviceId,
                  const char *command) {
    const uint32_t *once = durations.data() + code.at;
    const uint32_t *repeat = once + code.once;
    IrSignal s;
    s.carrierHz = code.carrierHz;
    s.first = code.once != 0 ? once : repeat;
    s.firstCount = static_cast<uint16_t>(code.once != 0 ? code.once : code.repeat);
    s.repeat = code.repeat != 0 ? repeat : once;
    s.repeatCount = static_cast<uint16_t>(code.repeat != 0 ? code.repeat : code.once);
    s.deviceId = deviceId;
    s.command = command;
    return s;
}

const char *irIntegrationState(const IntegrationSpec &integration, bool transmitterReady) {
    if (!integration.enabled) return "disabled";
    return transmitterReady ? "ready" : "not_ready";
}

std::string irIntegrationDetail(bool transmitterReady) {
    return transmitterReady ? std::string() : std::string("The IR emitter isn't working");
}

// One device's catalog: its profile's commands by name (the `command`
// enum), every code in microseconds, and its timing. Matched against the
// next device by what it sends, so it keeps no copy of the profile.
struct IrAdapter::DeviceCatalog : std::enable_shared_from_this<DeviceCatalog> {
    struct Entry {
        std::string name;
        std::optional<uint32_t> holdMs;
        IrCodeLayout code;
        std::optional<IrCodeLayout> toggle;
    };
    std::vector<Entry> entries;
    std::vector<uint32_t> durations;  // every code's, in one allocation
    uint16_t frames;
    uint32_t keyDelayMs;
    detail::RemoteGroup remote;  // names into entries[i].name
    CommandSpec specs[3];

    explicit DeviceCatalog(const DeviceProfile &profile) : frames(framesOf(profile)), keyDelayMs(keyDelayOf(profile)) {
        size_t total = 0;
        for (const ProfileCommand &c : profile.commands) {
            for (const ProntoRef *r : {&c.pronto, &c.prontoToggle}) total += r->empty() ? 0 : r->count - pronto::kHeaderWords;
        }
        durations.reserve(total);
        entries.reserve(profile.commands.size());
        for (const ProfileCommand &c : profile.commands) {
            Entry e;
            e.name = c.name;
            e.holdMs = c.holdMs;
            if (const uint16_t *w = profile.words(c.pronto)) e.code = appendIrCode(w, &durations);
            if (const uint16_t *w = profile.words(c.prontoToggle)) e.toggle = appendIrCode(w, &durations);
            entries.push_back(std::move(e));
        }
        remote.names.reserve(entries.size());
        for (const Entry &e : entries) remote.names.push_back(e.name.c_str());
        static_assert(static_cast<size_t>(IrCommand::Press) == 0 && static_cast<size_t>(IrCommand::PressUp) == 2,
                      "the remote group is the catalog");
        remote.fill(specs, this, kPressTimeoutMs);
    }
    DeviceCatalog(const DeviceCatalog &) = delete;
    DeviceCatalog &operator=(const DeviceCatalog &) = delete;

    // Sends the same codes, with the same names, holds and timing (labels aside).
    bool matches(const DeviceProfile &p) const {
        if (framesOf(p) != frames || keyDelayOf(p) != keyDelayMs || p.commands.size() != entries.size()) return false;
        for (size_t i = 0; i < entries.size(); i++) {
            const ProfileCommand &c = p.commands[i];
            const Entry &e = entries[i];
            const uint16_t *toggle = p.words(c.prontoToggle);
            if (c.name != e.name || c.holdMs != e.holdMs || !sameSignal(e.code, p.words(c.pronto), durations) ||
                (toggle != nullptr) != e.toggle.has_value() ||
                (toggle != nullptr && !sameSignal(*e.toggle, toggle, durations))) {
                return false;
            }
        }
        return true;
    }

    IrSignal signal(uint16_t command, bool toggled, const char *deviceId) const {
        const Entry &e = entries[command];
        return irSignal(toggled && e.toggle ? *e.toggle : e.code, durations, deviceId, e.name.c_str());
    }

    CommandCatalog catalog() const { return {specs, static_cast<uint16_t>(std::size(specs)), shared_from_this()}; }
};

const char *IrAdapter::integrationType() const { return kIrType; }

IrAdapter::IrAdapter(const char *integrationId, IrTransmitter &transmitter)
    : id_(integrationId), transmitter_(transmitter) {}

IrAdapter::~IrAdapter() = default;

CommandCatalog IrAdapter::commandsFor(const DeviceSpec &device) const {
    if (device.provider.type != kIrType || !device.profile || !device.profile->irWords) return {};
    std::lock_guard<std::mutex> lock(catalogsMutex_);
    // One catalog per distinct profile while anything uses it (see ir.h).
    catalogs_.erase(std::remove_if(catalogs_.begin(), catalogs_.end(),
                                   [](const std::weak_ptr<const DeviceCatalog> &c) { return c.expired(); }),
                    catalogs_.end());
    for (const auto &weak : catalogs_) {
        if (const auto c = weak.lock(); c != nullptr && c->matches(*device.profile)) return c->catalog();
    }
    const auto made = std::make_shared<const DeviceCatalog>(*device.profile);
    catalogs_.push_back(made);
    return made->catalog();
}

ExecResult IrAdapter::execute(const ExecRequest &request) {
    if (request.command == nullptr || request.command->data == nullptr || request.params == nullptr ||
        request.deviceId == nullptr) {
        return ExecResult::failed(ErrorCode::Internal);
    }
    const auto &catalog = *static_cast<const DeviceCatalog *>(request.command->data);
    const auto command = static_cast<IrCommand>(request.commandIndex);
    const uint16_t option = request.params[0].asOption();
    if (option >= catalog.entries.size()) return ExecResult::failed(ErrorCode::Internal);
    const std::string_view deviceId = request.deviceId;

    if (command == IrCommand::PressUp) {
        // Releases the matching hold, running or still waiting.
        const auto releases = [&](Job &j) {
            return j.inUse() && j.kind == Kind::Hold && !j.released && j.catalog.get() == &catalog && j.command == option &&
                   j.deviceId == deviceId;
        };
        if (releases(running_)) {
            running_.released = true;
            endHoldIfWaited();
        } else {
            for (size_t i = 0; i < queued_; i++) {
                if (releases(queue_[i])) {
                    queue_[i].released = true;
                    break;
                }
            }
        }
        return ExecResult::accepted();
    }

    if (!transmitter_.ready()) return ExecResult::failed(ErrorCode::AdapterUnavailable);
    if (queued_ == kQueueSize) return ExecResult::failed(ErrorCode::QueueFull);
    Job &job = queue_[queued_];
    job = Job{};
    if (!job.deviceId.assign(deviceId)) return ExecResult::failed(ErrorCode::Internal);
    job.catalog = catalog.shared_from_this();  // the job keeps it: work can outlive every config
    job.command = option;
    job.ticket = request.ticket;
    if (command == IrCommand::PressDown) {
        job.kind = Kind::Hold;
    } else {
        const bool stepHold = (request.paramsPresent & 0x2) != 0;
        job.holdMs = stepHold ? static_cast<uint32_t>(request.params[1].integer)
                              : catalog.entries[option].holdMs.value_or(0);
        job.kind = job.holdMs != 0 ? Kind::Timed : Kind::Tap;
        job.reports = !request.answerAtStart;
    }
    queued_++;
    const bool reports = job.reports;
    endHoldIfWaited();
    start(request.nowMs);
    return reports ? ExecResult::pending() : ExecResult::accepted();
}

void IrAdapter::endHoldIfWaited() {
    if (!running_.inUse() || running_.refused || running_.kind == Kind::Tap || running_.stopped) return;
    if (running_.released || queued_ > 0) {
        transmitter_.stop();
        running_.stopped = true;
    }
}

void IrAdapter::start(uint32_t nowMs) {
    if (running_.inUse() || queued_ == 0 || transmitter_.busy()) return;
    Job &next = queue_[0];
    DeviceState &device = state(next.deviceId.view(), nowMs);
    const DeviceCatalog &catalog = *next.catalog;
    if (!timeReached(nowMs, device.endedMs + catalog.keyDelayMs)) return;  // key delay: the queue waits
    running_ = next;
    removeQueued(0);
    running_.startedMs = nowMs;
    // A new press of a command with a toggle code flips its device's bit,
    // once the code is on its way (a refused or failed send leaves it).
    const bool flips = catalog.entries[running_.command].toggle.has_value();
    const IrSignal signal = catalog.signal(running_.command, flips && !device.toggle, running_.deviceId.c_str());
    // A hold released before its turn sends what a tap does.
    const bool hold = running_.kind == Kind::Timed || (running_.kind == Kind::Hold && !running_.released);
    running_.refused = !transmitter_.send(signal, catalog.frames, hold);  // poll() fails it
    if (!running_.refused && flips) {
        device.toggle = !device.toggle;
        running_.toggled = true;
    }
    endHoldIfWaited();
}

void IrAdapter::poll(uint32_t nowMs, CompletionSink &sink) {
    if (running_.refused) {
        if (running_.reports) sink.complete(running_.ticket, ExecResult::failed(ErrorCode::AdapterUnavailable), nowMs);
        running_ = Job{};
    }
    if (running_.inUse()) {
        if (running_.kind == Kind::Timed && !running_.released &&
            timeReached(nowMs, running_.startedMs + running_.holdMs)) {
            running_.released = true;
        }
        endHoldIfWaited();
        if (transmitter_.busy()) return;
        // Ended: sent, or failed on the way (the device didn't see a press).
        const bool failed = transmitter_.takeFailed();
        DeviceState &device = state(running_.deviceId.view(), nowMs);
        device.endedMs = nowMs;
        if (failed && running_.toggled) device.toggle = !device.toggle;
        const bool reports = running_.reports;
        const Ticket ticket = running_.ticket;
        running_ = Job{};
        if (reports) {
            sink.complete(ticket, failed ? ExecResult::failed(ErrorCode::AdapterFailed) : ExecResult::accepted(), nowMs);
        }
    }
    start(nowMs);
}

void IrAdapter::releaseAll(uint32_t nowMs) {
    (void)nowMs;
    // A binding's holds end; a timed hold is its sequence's (cancel() ends it).
    if (running_.inUse() && running_.kind == Kind::Hold) {
        running_.released = true;
        endHoldIfWaited();
    }
    for (size_t i = 0; i < queued_;) {
        if (queue_[i].kind == Kind::Hold && !queue_[i].released) {  // a released one waits its turn as a tap
            removeQueued(i);
        } else {
            i++;
        }
    }
}

void IrAdapter::cancel(Ticket ticket, uint32_t nowMs) {
    (void)nowMs;
    for (size_t i = 0; i < queued_; i++) {
        if (queue_[i].reports && queue_[i].ticket == ticket) return removeQueued(i);
    }
    if (running_.inUse() && running_.reports && running_.ticket == ticket) {
        running_.reports = false;  // a frame in flight finishes; nobody waits for it
        running_.released = true;
        endHoldIfWaited();
    }
}

void IrAdapter::reset() {
    for (Job &j : queue_) j = Job{};
    queued_ = 0;
    running_ = Job{};
    for (DeviceState &d : devices_) d = DeviceState{};
}

void IrAdapter::removeQueued(size_t index) {
    for (size_t i = index; i + 1 < queued_; i++) queue_[i] = queue_[i + 1];
    queue_[--queued_] = Job{};
}

IrAdapter::DeviceState &IrAdapter::state(std::string_view deviceId, uint32_t nowMs) {
    DeviceState *free = nullptr;
    DeviceState *oldest = &devices_[0];
    for (DeviceState &d : devices_) {
        if (d.deviceId == deviceId && !d.deviceId.empty()) return d;
        if (d.deviceId.empty()) {
            if (free == nullptr) free = &d;
        } else if (static_cast<int32_t>(d.endedMs - oldest->endedMs) < 0) {
            oldest = &d;
        }
    }
    DeviceState &slot = free != nullptr ? *free : *oldest;  // more devices than slots: the longest idle goes
    slot = DeviceState{};
    (void)slot.deviceId.assign(deviceId);
    // Long enough ago that no key delay applies.
    slot.endedMs = nowMs - limits::kMaxKeyDelayMs - 1;
    return slot;
}

bool IrAdapter::toggle(std::string_view deviceId) const {
    for (const DeviceState &d : devices_) {
        if (!d.deviceId.empty() && d.deviceId == deviceId) return d.toggle;
    }
    return false;
}

}  // namespace hubbo::integrations
