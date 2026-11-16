#include "hubbo/engine/engine.h"

#include <algorithm>

#include "hubbo/domain/time.h"

namespace hubbo {

const char *phaseStatusName(PhaseStatus status) {
    switch (status) {
        case PhaseStatus::None: return "none";
        case PhaseStatus::Queued: return "queued";
        case PhaseStatus::Rejected: return "rejected";
    }
    return "none";
}

namespace {

constexpr int kMaxChainedTransitions = 4;  // defensive; the compiler forbids chains

}  // namespace

Engine::Engine(Store &store, AdapterSet &adapters, uint32_t actionTimeoutMs)
    : store_(store), adapters_(adapters), executor_(adapters, actionTimeoutMs) {
    store_.attach(reader_);
    relay_.engine = this;
    executor_.setObserver(&relay_);
}

void Engine::setObserver(EngineObserver *observer) { observer_ = observer; }

void Engine::Relay::onActionDispatched(const DispatchRecord &record) {
    if (engine->history_ != nullptr) engine->history_->onDispatched(record);
    if (engine->observer_ != nullptr) engine->observer_->onActionDispatched(record);
    engine->trackHolds(record);
}

void Engine::Relay::onActionCompleted(const CompletionRecord &record) {
    if (engine->history_ != nullptr) engine->history_->onCompleted(record);
    engine->resolveRelease(record);
    if (engine->observer_ != nullptr) engine->observer_->onActionCompleted(record);
}

void Engine::Relay::onSequenceFinished(const SequenceReport &report) {
    if (engine->history_ != nullptr) engine->history_->onFinished(report);
    engine->resolveReleases(report);
    if (report.info.origin == SequenceOrigin::Api) engine->finishCommand(report);
    const bool binding =
        report.info.origin == SequenceOrigin::Binding || report.info.origin == SequenceOrigin::GlobalBinding;
    if (binding && report.info.trigger.kind == remote::EventKind::Up) {
        const auto b = static_cast<size_t>(report.info.trigger.button);
        if (engine->upSequence_[b] == report.info.id) {
            engine->upSequence_[b] = 0;
            engine->releaseHolds(report.info.trigger.button);  // whatever the up sequence did
        }
    }
    if (engine->observer_ != nullptr) engine->observer_->onSequenceFinished(report);
}

void Engine::advanceClock(uint32_t nowMs) {
    // Monotonic: event times can lag "now" (hold/timeout events are stamped
    // with when they happened), so never step backwards.
    if (!clockStarted_ || timeAfter(nowMs, nowMs_)) {
        nowMs_ = nowMs;
        clockStarted_ = true;
    }
}

void Engine::onRemoteEvent(const remote::RemoteEvent &event) {
    advanceClock(event.atMs);
    stats_.events++;
    const remote::ButtonSet bit = remote::buttonBit(event.button);
    const bool up = event.kind == remote::EventKind::Up;
    const bool afterHold = up && (holdFired_ & bit) != 0;
    if (event.kind == remote::EventKind::Down) held_ |= bit;
    if (event.kind == remote::EventKind::Down || up) holdFired_ &= ~bit;
    if (event.kind == remote::EventKind::Hold) holdFired_ |= bit;
    if (up) held_ &= ~bit;
    bool upQueued = false;
    {
        auto guard = reader_.read();
        const CompiledConfig *config = guard.get();
        if (config == nullptr) {
            stats_.unbound++;
            if (observer_ != nullptr) observer_->onUnbound(event);
            if (up) releaseHolds(event.button);
            return;
        }
        syncConfig(guard);
        applyPending(config);  // a deleted activity's fallback, before the lookup

        // A smart press is picked at Down and kept until Up (see engine.h).
        SmartRef &pressed = pressed_[static_cast<size_t>(event.button)];
        if (event.kind == remote::EventKind::Down) {
            pressed = {};
            SmartSlot slot;
            SmartPress which;
            if (!config->activityBinds(activeIndex_, event.button)) {
                if (smartSlotForButton(event.button, &slot)) {
                    if (slot != selectedSlotShared()) flushStep();
                    selected_.store(static_cast<uint8_t>(slot), std::memory_order_relaxed);
                }
                if (config->smartPress(event.button, selectedSlotShared(), &slot, &which) != nullptr) {
                    pressed = {true, static_cast<uint8_t>(slot), which};
                }
            }
        }
        // Kept past Up: the Tap comes right after it. The next Down replaces it.
        const CompiledSmartPress *smart = pressed.set ? &pressed.in(*config) : nullptr;
        const bool plusMinus = event.button == remote::Button::Plus || event.button == remote::Button::Minus;
        if (plusMinus && event.kind == remote::EventKind::Hold) flushStep();
        if (smart != nullptr && smart->merges && event.kind == remote::EventKind::Tap && addStepTap(*config, pressed)) {
            pumpAndApply(config);  // a pending tap the other way may have gone
            return;                // this one goes when the taps stop (poll())
        }
        bool global = false;
        ActionSpan span;
        if (smart != nullptr) {
            switch (event.kind) {
                case remote::EventKind::Tap: span = smart->tap; break;
                case remote::EventKind::Hold: span = smart->hold; break;
                case remote::EventKind::Up: span = afterHold ? smart->release : ActionSpan{}; break;
                default: break;
            }
        } else {
            span = config->lookup(activeIndex_, event.button, event.kind, &global);
        }
        if (span.count == 0) {
            stats_.unbound++;
            if (observer_ != nullptr) observer_->onUnbound(event);
        } else {
            SequenceInfo info;
            info.lane = Lane::Input;
            info.origin = smart != nullptr ? SequenceOrigin::SmartButton
                          : global         ? SequenceOrigin::GlobalBinding
                                           : SequenceOrigin::Binding;
            info.trigger = Trigger{event.button, event.kind};
            if (info.origin == SequenceOrigin::Binding) info.activityId = activeId_;
            info.generation = generation_;
            uint32_t sequenceId = 0;
            const ErrorCode result = executor_.enqueue(info, *config, span, nowMs_, &sequenceId);
            if (result == ErrorCode::Ok) {
                stats_.dispatched++;
                // A binding's up sequence releases the button's holds when it
                // ends; a smart button's has none (no hold commands there).
                if (up && smart == nullptr) {
                    upSequence_[static_cast<size_t>(event.button)] = sequenceId;
                    upQueued = true;
                }
            }
        }
        if (up && !upQueued) releaseHolds(event.button);  // unbound, rejected or stale: release now
        pumpAndApply(config);
    }
    if (event.timedOut && event.kind == remote::EventKind::Up) {
        adapters_.releaseAll(nowMs_);
    }
}

ErrorCode Engine::activate(std::string_view activityId, uint32_t nowMs, TransitionResult *result) {
    advanceClock(nowMs);
    const TransitionRequest request = TransitionRequest::activate(activityId);
    auto guard = reader_.read();
    const CompiledConfig *config = guard.get();
    if (config == nullptr) return reject(ErrorCode::UnknownActivity, request.activityId.c_str(), result);
    syncConfig(guard);
    if (request && config->findActivity(activityId) >= 0) {
        pending_ = {};  // an explicit, valid target replaces a deleted-activity fallback
    }
    applyPending(config);
    const ErrorCode code = apply(*config, request, result);
    pumpAndApply(config);
    return code;
}

ErrorCode Engine::restoreActive(std::string_view activityId, uint32_t nowMs) {
    advanceClock(nowMs);
    auto guard = reader_.read();
    const CompiledConfig *config = guard.get();
    if (config == nullptr) return ErrorCode::UnknownActivity;
    syncConfig(guard);
    const int index = config->findActivity(activityId);
    if (index < 0) return ErrorCode::UnknownActivity;
    pending_ = {};  // nothing else should run at boot
    activeIndex_ = index;
    activeId_ = config->activities[static_cast<size_t>(index)].id;
    publishActive();
    if (observer_ != nullptr) observer_->onActivityChanged(activeId_.c_str(), nowMs_);
    return ErrorCode::Ok;
}

// A seqlock without fences (ThreadSanitizer can't model fences): each
// character store is a release, so it can't move above the "writing" mark;
// each character load is an acquire, so the closing sequence check can't
// move above it. A reader that sees any new character therefore sees the
// sequence change and retries.
void Engine::SharedId::store(std::string_view id) {
    seq_.fetch_add(1, std::memory_order_relaxed);  // odd: writing
    for (size_t i = 0; i <= limits::kMaxIdLength; i++) {
        chars_[i].store(i < id.size() ? id[i] : '\0', std::memory_order_release);
    }
    seq_.fetch_add(1, std::memory_order_release);  // even: stable
}

std::string Engine::SharedId::load() const {
    char buf[limits::kMaxIdLength + 1];
    for (;;) {
        const uint32_t before = seq_.load(std::memory_order_acquire);
        if ((before & 1u) != 0) continue;
        for (size_t i = 0; i <= limits::kMaxIdLength; i++) buf[i] = chars_[i].load(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) == before) break;
    }
    buf[limits::kMaxIdLength] = '\0';
    return std::string(buf);
}

ErrorCode Engine::off(uint32_t nowMs, TransitionResult *result) {
    advanceClock(nowMs);
    auto guard = reader_.read();
    const CompiledConfig *config = guard.get();
    if (config != nullptr) {
        syncConfig(guard);
        if (pending_) {
            // Off was asked for: the active activity was just deleted, so
            // there is nothing to shut down and no replacement to start.
            pending_ = {};
            if (observer_ != nullptr) observer_->onActivityChanged("", nowMs_);
        }
    }
    if (config == nullptr) return reject(ErrorCode::NoActiveActivity, "", result);
    const ErrorCode code = apply(*config, TransitionRequest::off(), result);
    pumpAndApply(config);
    return code;
}

void Engine::poll(uint32_t nowMs) {
    advanceClock(nowMs);
    EngineRequest *pending = nullptr;
    while (requests_.pop(&pending)) handle(*pending);
    adapters_.poll(nowMs_, executor_);
    auto guard = reader_.read();
    if (guard) syncConfig(guard);
    const bool stepHeld = (held_ & remote::buttonBit(step_.button())) != 0;
    if (step_.count != 0 && !stepHeld && timeReached(nowMs_, step_.dueAtMs)) flushStep();
    pumpAndApply(guard.get());
    if (history_ != nullptr) history_->publish();
}

bool Engine::addStepTap(const CompiledConfig &config, const SmartRef &pressed) {
    if (step_.count != 0 && (step_.press.press != pressed.press || step_.press.slot != pressed.slot)) {
        flushStep();
    }
    if (step_.count == 0) {
        // Merging presses have one tap action (Compiler::smartPress).
        const CompiledAction &action = *config.span(pressed.in(config).tap);
        step_.command.action = action;
        step_.command.deviceId.clear();
        step_.command.providerRef.clear();
        if (const CompiledDevice *device = config.deviceOf(action)) {
            step_.command.deviceId = device->id;
            step_.command.providerRef = device->providerRef;
        }
    }
    step_.press = pressed;
    if (step_.count < UINT16_MAX) step_.count++;
    step_.dueAtMs = nowMs_ + kSmartStepWindowMs;
    return true;
}

void Engine::endActivity() {
    generation_++;
    stats_.transitions++;
    executor_.cancelAll(generation_, nowMs_);
    flushStep();  // after the cancel, so it isn't dropped
    releaseAllHolds();
    adapters_.releaseAll(nowMs_);
}

void Engine::flushStep() {
    const int32_t taps = step_.count;
    step_.count = 0;
    if (taps == 0) return;
    CompiledCommand &command = step_.command;
    const auto step = static_cast<size_t>(command.action.stepParam);
    const ParamSpec &spec = command.action.command->params[step];
    ParamValue &value = command.action.params[step];
    value.integer = static_cast<int32_t>(
        std::clamp<int64_t>(static_cast<int64_t>(value.integer) * taps, spec.minInt, spec.maxInt));
    SequenceInfo info;
    info.lane = Lane::Input;
    info.origin = SequenceOrigin::SmartButton;
    info.trigger = Trigger{step_.button(), remote::EventKind::Tap};
    info.generation = generation_;
    if (executor_.enqueueCommand(info, command, nowMs_) == ErrorCode::Ok) stats_.dispatched++;
}

bool Engine::submit(EngineRequest &request) {
    request.done_.store(false, std::memory_order_relaxed);
    return requests_.push(&request);
}

void Engine::handle(EngineRequest &request) {
    switch (request.kind_) {
        case EngineRequest::Kind::Activate:
            activate(request.activityId_.view(), nowMs_, &request.transition_);
            break;
        case EngineRequest::Kind::Off:
            off(nowMs_, &request.transition_);
            break;
        case EngineRequest::Kind::Execute: {
            request.startedMs_ = nowMs_;
            EngineRequest **slot = nullptr;
            for (auto &p : pendingCommands_) {
                if (p == nullptr) {
                    slot = &p;
                    break;
                }
            }
            SequenceInfo info;
            info.lane = Lane::Input;
            info.origin = SequenceOrigin::Api;
            info.generation = generation_;
            const ErrorCode queued = slot == nullptr ? ErrorCode::QueueFull
                                                     : executor_.enqueueCommand(info, request.command_, nowMs_,
                                                                                &request.sequenceId_);
            if (queued != ErrorCode::Ok) {
                request.result_ = {ExecStatus::Failed, queued, 0};
                break;
            }
            *slot = &request;  // finishCommand() completes it
            auto guard = reader_.read();
            pumpAndApply(guard.get());
            return;
        }
    }
    request.done_.store(true, std::memory_order_release);
}

void Engine::finishCommand(const SequenceReport &report) {
    for (auto &p : pendingCommands_) {
        if (p == nullptr || p->sequenceId_ != report.info.id) continue;
        CommandResult &r = p->result_;
        r.elapsedMs = nowMs_ - p->startedMs_;
        r.error = report.error;
        if (report.outcome == SequenceOutcome::Completed) {
            r.status = ExecStatus::Accepted;
        } else if (report.error == ErrorCode::AdapterTimeout) {
            r.status = ExecStatus::Timeout;
        } else {
            r.status = ExecStatus::Failed;
        }
        EngineRequest *done = p;
        p = nullptr;
        done->done_.store(true, std::memory_order_release);
        return;
    }
}

void Engine::syncConfig(const Store::Guard &guard) {
    if (guard.serial() == seenSerial_ || guard.get() == nullptr) return;
    seenSerial_ = guard.serial();
    flushStep();  // as tapped, before anything uses the new config
    const CompiledConfig &config = *guard.get();
    if (activeIndex_ >= 0) {
        const int index = config.findActivity(activeId_.view());
        if (index >= 0) {
            activeIndex_ = index;
        } else {
            // The active activity is gone. Nothing shuts down. Deleted by the
            // user: fall back to the next activity (the one that took its
            // place in the list, wrapping to the first), whose startup runs.
            // Gone any other way (restore, reset, a reload): inactive, and
            // nothing starts (design part 6). Its queued work and holds end
            // like on any change of activity.
            const bool deleted = config.deletedActivity.view() == activeId_.view();
            const auto count = static_cast<int>(config.activities.size());
            const int next = activeIndex_ < count ? activeIndex_ : 0;
            activeIndex_ = -1;
            activeId_.clear();
            publishActive();
            if (deleted && count > 0) {
                pending_ = TransitionRequest::activate(config.activities[static_cast<size_t>(next)].id.view());
            } else {
                endActivity();
                if (observer_ != nullptr) observer_->onActivityChanged("", nowMs_);
            }
        }
    }
    if (observer_ != nullptr) observer_->onConfigChanged(config.revision, nowMs_);
}

ErrorCode Engine::reject(ErrorCode code, const char *activityId, TransitionResult *result) {
    if (observer_ != nullptr) observer_->onTransitionRejected(code, activityId, nowMs_);
    if (result != nullptr) {
        *result = {};
        result->error = code;
        result->activeActivityId = activeId_;
        result->generation = generation_;
    }
    return code;
}

ErrorCode Engine::apply(const CompiledConfig &config, const TransitionRequest &request, TransitionResult *result) {
    if (request.kind == TransitionRequest::Kind::Off) {
        if (activeIndex_ < 0) return reject(ErrorCode::NoActiveActivity, "", result);
        return transition(config, -1, result);
    }
    const int index = request ? config.findActivity(request.activityId.view()) : -1;
    if (index < 0) return reject(ErrorCode::UnknownActivity, request.activityId.c_str(), result);
    return transition(config, index, result);
}

void Engine::applyPending(const CompiledConfig *config) {
    if (!pending_ || config == nullptr) return;
    const TransitionRequest request = pending_;
    pending_ = {};
    apply(*config, request);
}

ErrorCode Engine::transition(const CompiledConfig &config, int targetIndex, TransitionResult *result) {
    const IdString previous = activeId_;
    endActivity();

    SequenceInfo info;
    info.lane = Lane::Transition;
    info.generation = generation_;
    ActionSpan span;
    if (targetIndex >= 0) {
        activeIndex_ = targetIndex;
        activeId_ = config.activities[static_cast<size_t>(targetIndex)].id;
        info.origin = SequenceOrigin::Startup;
        info.activityId = activeId_;
        span = config.activities[static_cast<size_t>(targetIndex)].startup;
    } else {
        info.origin = SequenceOrigin::Shutdown;
        info.activityId = activeId_;
        span = config.activities[static_cast<size_t>(activeIndex_)].shutdown;
        activeIndex_ = -1;
        activeId_.clear();
    }
    publishActive();
    if (observer_ != nullptr) observer_->onActivityChanged(activeId_.c_str(), nowMs_);
    uint32_t sequenceId = 0;
    const ErrorCode queued = executor_.enqueue(info, config, span, nowMs_, &sequenceId);
    if (history_ != nullptr && span.count != 0) {
        info.id = sequenceId;
        history_->begin(info, config, span, queued, nowMs_);
    }
    if (result != nullptr) {
        *result = {};
        result->switched = true;
        result->activeActivityId = activeId_;
        result->previousActivityId = previous;
        result->generation = generation_;
        TransitionPhase &phase = targetIndex >= 0 ? result->startup : result->shutdown;
        if (queued != ErrorCode::Ok) {
            phase.status = PhaseStatus::Rejected;
            phase.reason = queued;
        } else if (span.count > 0) {
            phase.status = PhaseStatus::Queued;
            phase.actions = span.count;
            result->queuedActions = span.count;
        }
    }
    // It switched: that's the outcome, whatever happened to the sequence.
    return ErrorCode::Ok;
}

void Engine::pumpAndApply(const CompiledConfig *config) {
    // A request left over after the bound (impossible with compiler-checked
    // configs) stays in pending_ for the next call.
    for (int i = 0; i < kMaxChainedTransitions; i++) {
        applyPending(config);
        executor_.pump(nowMs_);
        pending_ = executor_.takeTransition();
        if (!pending_) return;
    }
}

void Engine::trackHolds(const DispatchRecord &record) {
    const CompiledAction &action = record.action;
    if (action.op != ActionOp::Adapter || action.command == nullptr) return;
    const bool reached = succeeded(record.result.status) || record.result.status == ExecStatus::Pending;
    if (!reached) return;
    if (action.command->effect != CommandEffect::Hold) {
        // A binding's own release. The hold closes only once the release has
        // completed: at once if it succeeded; if Pending, with its own result
        // (resolveRelease()).
        const int slot = holds_.match(action.adapterIndex, record.target, action.command, action.params);
        if (slot < 0) return;
        if (succeeded(record.result.status)) {
            holds_.erase(slot);
        } else {
            holds_.markReleasing(slot, record.sequence.id, record.index);
        }
        return;
    }
    const bool binding = record.sequence.origin == SequenceOrigin::Binding ||
                         record.sequence.origin == SequenceOrigin::GlobalBinding;
    if (!binding || action.release == nullptr) return;  // the compiler allows holds nowhere else
    OpenHold hold;
    hold.button = record.sequence.trigger.button;
    hold.adapterIndex = action.adapterIndex;
    hold.release = action.release;
    hold.releaseIndex = action.releaseIndex;
    for (size_t i = 0; i < limits::kMaxParams; i++) hold.params[i] = action.params[i];
    hold.paramsPresent = action.paramsPresent;
    hold.deviceId.assign(record.target);
    hold.providerRef.assign(record.providerRef);
    // Its button is already up (the hold ran late), or the ledger is full:
    // release it now rather than risk a stuck key.
    if ((held_ & remote::buttonBit(hold.button)) == 0 || !holds_.add(hold)) releaseHold(hold);
}

void Engine::resolveRelease(const CompletionRecord &record) {
    // A pending release's own result: it went (the hold is closed) or it may
    // not have (failed or timed out: the adapter's releaseAll).
    OpenHold hold;
    if (!holds_.takeReleasing(record.sequence.id, record.index, &hold)) return;
    if (!succeeded(record.result.status)) forceRelease(hold);
}

void Engine::resolveReleases(const SequenceReport &report) {
    // A release still pending when its sequence ends never got a result
    // (cancelled): unknown, so the adapter's releaseAll.
    OpenHold hold;
    while (holds_.takeReleasing(report.info.id, -1, &hold)) forceRelease(hold);
}

void Engine::forceRelease(const OpenHold &hold) {
    stats_.forcedReleases++;
    if (IntegrationAdapter *adapter = adapters_.at(hold.adapterIndex)) adapter->releaseAll(nowMs_);
}

void Engine::releaseHolds(remote::Button button) {
    OpenHold hold;
    while (holds_.take(button, &hold)) releaseHold(hold);
}

void Engine::releaseAllHolds() {
    OpenHold hold;
    while (holds_.takeAny(&hold)) releaseHold(hold);
}

void Engine::releaseHold(const OpenHold &hold) {
    IntegrationAdapter *adapter = adapters_.at(hold.adapterIndex);
    if (adapter == nullptr) return;
    stats_.forcedReleases++;
    const ExecRequest request{hold.release, hold.releaseIndex, hold.params, hold.paramsPresent,
                              hold.deviceId.c_str(), hold.providerRef.c_str(), /*ticket=*/0, nowMs_,
                              /*answerAtStart=*/false};
    const ExecResult result = adapter->execute(request);
    // Not confirmed: failed, timed out, or Pending (this direct execution has
    // no ticket, so its result can't be tracked). Fall back to releaseAll.
    if (result.status != ExecStatus::Accepted) adapter->releaseAll(nowMs_);
    if (observer_ != nullptr) {
        // Reported like a binding's action on the button's up.
        SequenceInfo info;
        info.origin = SequenceOrigin::Binding;
        info.trigger = Trigger{hold.button, remote::EventKind::Up};
        info.activityId = activeId_;
        info.generation = generation_;
        CompiledAction action;
        action.op = ActionOp::Adapter;
        action.adapterIndex = hold.adapterIndex;
        action.command = hold.release;
        action.commandIndex = hold.releaseIndex;
        for (size_t i = 0; i < limits::kMaxParams; i++) action.params[i] = hold.params[i];
        action.paramsPresent = hold.paramsPresent;
        observer_->onActionDispatched({info, 0, action, adapter->integrationId(), hold.deviceId.c_str(),
                                       hold.providerRef.c_str(), result, nowMs_});
    }
}

}  // namespace hubbo
