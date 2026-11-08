#include "hubbo/engine/engine.h"

#include "hubbo/domain/time.h"

namespace hubbo {

namespace {

constexpr int kMaxChainedTransitions = 4;  // defensive; the compiler forbids chains

}  // namespace

Engine::Engine(Store &store, AdapterSet &adapters, uint32_t actionTimeoutMs)
    : store_(store), adapters_(adapters), executor_(adapters, actionTimeoutMs) {
    store_.attach(reader_);
}

void Engine::setObserver(EngineObserver *observer) {
    observer_ = observer;
    executor_.setObserver(observer);
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
    {
        auto guard = reader_.read();
        const CompiledConfig *config = guard.get();
        if (config == nullptr) {
            stats_.unbound++;
            if (observer_ != nullptr) observer_->onUnbound(event);
            return;
        }
        syncConfig(guard);
        applyPending(config);  // a deleted activity's fallback, before the lookup

        bool global = false;
        const ActionSpan span = config->lookup(activeIndex_, event.button, event.kind, &global);
        if (span.count == 0) {
            stats_.unbound++;
            if (observer_ != nullptr) observer_->onUnbound(event);
        } else {
            SequenceInfo info;
            info.lane = Lane::Input;
            info.origin = global ? SequenceOrigin::GlobalBinding : SequenceOrigin::Binding;
            info.trigger = Trigger{event.button, event.kind};
            if (!global) info.activityId = activeId_;
            info.generation = generation_;
            const ErrorCode result = executor_.enqueue(info, *config, span, nowMs_);
            if (result == ErrorCode::Ok) {
                stats_.dispatched++;
            } else if (result == ErrorCode::QueueFull &&
                       (event.kind == remote::EventKind::Up || event.kind == remote::EventKind::Tap)) {
                stats_.overflowReleases++;
                releaseTargets(*config, span);
            }
        }
        pumpAndApply(config);
    }
    if (event.timedOut && event.kind == remote::EventKind::Up) {
        adapters_.releaseAll(nowMs_);
    }
}

ErrorCode Engine::activate(std::string_view activityId, uint32_t nowMs) {
    advanceClock(nowMs);
    const TransitionRequest request = TransitionRequest::activate(activityId);
    auto guard = reader_.read();
    const CompiledConfig *config = guard.get();
    if (config == nullptr) {
        if (observer_ != nullptr) {
            observer_->onTransitionRejected(ErrorCode::UnknownActivity, request.activityId.c_str(), nowMs_);
        }
        return ErrorCode::UnknownActivity;
    }
    syncConfig(guard);
    if (request && config->findActivity(activityId) >= 0) {
        pending_ = {};  // an explicit, valid target replaces a deleted-activity fallback
    }
    applyPending(config);
    const ErrorCode result = apply(*config, request);
    pumpAndApply(config);
    return result;
}

ErrorCode Engine::off(uint32_t nowMs) {
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
    if (config == nullptr) {
        if (observer_ != nullptr) observer_->onTransitionRejected(ErrorCode::NoActiveActivity, "", nowMs_);
        return ErrorCode::NoActiveActivity;
    }
    const ErrorCode result = apply(*config, TransitionRequest::off());
    pumpAndApply(config);
    return result;
}

void Engine::poll(uint32_t nowMs) {
    advanceClock(nowMs);
    TransitionRequest request;
    while (mailbox_.pop(&request)) {
        if (request.kind == TransitionRequest::Kind::Activate) {
            activate(request.activityId.view(), nowMs_);
        } else {
            off(nowMs_);
        }
    }
    adapters_.poll(nowMs_, executor_);
    auto guard = reader_.read();
    if (guard) syncConfig(guard);
    pumpAndApply(guard.get());
}

bool Engine::postActivate(std::string_view activityId) {
    const TransitionRequest request = TransitionRequest::activate(activityId);
    return request && mailbox_.push(request);
}

bool Engine::postOff() { return mailbox_.push(TransitionRequest::off()); }

void Engine::syncConfig(const Store::Guard &guard) {
    if (guard.serial() == seenSerial_ || guard.get() == nullptr) return;
    seenSerial_ = guard.serial();
    const CompiledConfig &config = *guard.get();
    if (activeIndex_ >= 0) {
        const int index = config.findActivity(activeId_.view());
        if (index >= 0) {
            activeIndex_ = index;
        } else {
            // Deleted while active. Fall back to the next activity (the one
            // that took its place in the list, wrapping to the first); its
            // startup runs, nothing shuts down. Inactive if none remain.
            const auto count = static_cast<int>(config.activities.size());
            const int next = activeIndex_ < count ? activeIndex_ : 0;
            activeIndex_ = -1;
            activeId_.clear();
            if (count > 0) {
                pending_ = TransitionRequest::activate(config.activities[static_cast<size_t>(next)].id.view());
            } else if (observer_ != nullptr) {
                observer_->onActivityChanged("", nowMs_);
            }
        }
    }
    if (observer_ != nullptr) observer_->onConfigChanged(config.revision, nowMs_);
}

ErrorCode Engine::apply(const CompiledConfig &config, const TransitionRequest &request) {
    if (request.kind == TransitionRequest::Kind::Off) {
        if (activeIndex_ < 0) {
            if (observer_ != nullptr) observer_->onTransitionRejected(ErrorCode::NoActiveActivity, "", nowMs_);
            return ErrorCode::NoActiveActivity;
        }
        return transition(config, -1);
    }
    const int index = request ? config.findActivity(request.activityId.view()) : -1;
    if (index < 0) {
        if (observer_ != nullptr) {
            observer_->onTransitionRejected(ErrorCode::UnknownActivity, request.activityId.c_str(), nowMs_);
        }
        return ErrorCode::UnknownActivity;
    }
    return transition(config, index);
}

void Engine::applyPending(const CompiledConfig *config) {
    if (!pending_ || config == nullptr) return;
    const TransitionRequest request = pending_;
    pending_ = {};
    apply(*config, request);
}

ErrorCode Engine::transition(const CompiledConfig &config, int targetIndex) {
    generation_++;
    stats_.transitions++;
    executor_.cancelAll(generation_, nowMs_);
    adapters_.releaseAll(nowMs_);

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
    if (observer_ != nullptr) observer_->onActivityChanged(activeId_.c_str(), nowMs_);
    return executor_.enqueue(info, config, span, nowMs_);
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

void Engine::releaseTargets(const CompiledConfig &config, ActionSpan span) {
    const CompiledAction *actions = config.span(span);
    for (uint8_t i = 0; i < span.count; i++) {
        if (actions[i].op != ActionOp::Adapter) continue;
        if (IntegrationAdapter *adapter = adapters_.at(actions[i].adapterIndex)) {
            adapter->releaseAll(nowMs_);
        }
    }
}

}  // namespace hubbo
