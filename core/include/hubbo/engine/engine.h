#pragma once

#include <cstdint>
#include <string_view>

#include "hubbo/domain/adapter.h"
#include "hubbo/domain/ids.h"
#include "hubbo/engine/compiled.h"
#include "hubbo/engine/executor.h"
#include "hubbo/engine/snapshot.h"
#include "hubbo/engine/spsc.h"
#include "hubbo/remote/input.h"

// The activity engine: the remote's brain. Owns the active activity, turns
// remote events into queued sequences, and runs activity transitions.
//
// Threading. Everything except postActivate()/postOff() runs on one task
// (the engine/RF task). Config arrives from another task through the
// SnapshotStore; the engine reads it through its own Reader, pinning a
// snapshot only for the duration of one call.
//
// Hot path (onRemoteEvent): pin snapshot -> two array indexes -> copy the
// span into the executor -> pump. No allocation, JSON, strings or I/O
// (except one id lookup per config swap to re-find the active activity).
//
// Lifecycle (ARCHITECTURE "Activity switching"):
//   activate(B): generation++, cancel every queued/parked sequence (so A's
//     startup can't resume), releaseAll() on every adapter (no held HID key
//     survives a switch), select B's table, queue B's startup. A's shutdown
//     never runs. Activating the already-active activity re-runs its startup.
//   off(): generation++, cancel, releaseAll, queue the current activity's
//     shutdown, clear the active activity (only global bindings apply
//     while it runs). NoActiveActivity if nothing is active.
//   Config swap: the active activity is re-found by id. If it was deleted,
//     the hub queues a fallback request to activate the next activity (the
//     one now at its list position, wrapping to the first); no shutdown runs.
//     An explicit request replaces the fallback: activate(valid id) wins, and
//     off() drops it and leaves the hub inactive. Inactive too if no
//     activities remain. Queued sequences keep running (they hold copies).
//
// Held-key safety on top of the executor:
//   * every transition calls releaseAll() on all adapters;
//   * an Up/Tap event that was synthesized after a lost release (timedOut)
//     also triggers releaseAll() after dispatch;
//   * if an Up/Tap sequence is rejected for overflow, releaseAll() is called
//     on the adapters that sequence targeted, so a key_up can't be lost.
namespace hubbo {

class EngineObserver : public ExecutionObserver {
public:
    // id is "" when the hub becomes inactive.
    virtual void onActivityChanged(const char *activityId, uint32_t nowMs) {
        (void)activityId;
        (void)nowMs;
    }
    // A requested transition (button, API or post*) that could not run.
    virtual void onTransitionRejected(ErrorCode error, const char *activityId, uint32_t nowMs) {
        (void)error;
        (void)activityId;
        (void)nowMs;
    }
    virtual void onConfigChanged(uint32_t revision, uint32_t nowMs) {
        (void)revision;
        (void)nowMs;
    }
    // An event with no binding in the active activity or globals.
    virtual void onUnbound(const remote::RemoteEvent &event) { (void)event; }
};

struct EngineStats {
    uint32_t events = 0;
    uint32_t unbound = 0;
    uint32_t dispatched = 0;
    uint32_t overflowReleases = 0;
    uint32_t transitions = 0;
};

class Engine : public remote::EventSink {
public:
    using Store = SnapshotStore<CompiledConfig>;

    Engine(Store &store, AdapterSet &adapters, uint32_t actionTimeoutMs = limits::kDefaultActionTimeoutMs);
    ~Engine() override = default;

    // False if the store had no free reader slot (engine is then inert).
    bool ready() const { return reader_.attached(); }

    void setObserver(EngineObserver *observer);

    // RemoteInput sink (engine task). Event time is taken as "now".
    void onRemoteEvent(const remote::RemoteEvent &event) override;

    // Engine task. Return Ok, UnknownActivity, NoActiveActivity (off) or
    // QueueFull (the switch happened but the sequence was rejected).
    ErrorCode activate(std::string_view activityId, uint32_t nowMs);
    ErrorCode off(uint32_t nowMs);

    // Engine task, every loop iteration: drains the mailbox, delivers adapter
    // completions, resumes waits, applies config swaps.
    void poll(uint32_t nowMs);

    // Any one other task (e.g. HTTP): queue a request for the next poll().
    // Returns false if the mailbox is full. Results arrive via the observer.
    bool postActivate(std::string_view activityId);
    bool postOff();

    // Engine-task state.
    bool hasActiveActivity() const { return activeIndex_ >= 0; }
    const char *activeActivityId() const { return activeId_.c_str(); }
    uint32_t generation() const { return generation_; }
    uint32_t nowMs() const { return nowMs_; }

    Executor &executor() { return executor_; }
    const Executor &executor() const { return executor_; }
    const EngineStats &stats() const { return stats_; }

private:
    void advanceClock(uint32_t nowMs);
    // Notices a new config. A deleted active activity leaves a fallback
    // request in pending_ (or reports inactive if none remain).
    void syncConfig(const Store::Guard &guard);
    // The one place requests become transitions (or rejections).
    ErrorCode apply(const CompiledConfig &config, const TransitionRequest &request);
    void applyPending(const CompiledConfig *config);
    ErrorCode transition(const CompiledConfig &config, int targetIndex);
    // Pump the executor, applying pending and system.activate/off requests.
    void pumpAndApply(const CompiledConfig *config);
    void releaseTargets(const CompiledConfig &config, ActionSpan span);

    Store &store_;
    Store::Reader reader_;
    AdapterSet &adapters_;
    Executor executor_;
    EngineObserver *observer_ = nullptr;

    int activeIndex_ = -1;
    IdString activeId_;
    uint32_t seenSerial_ = 0;
    uint32_t generation_ = 0;
    uint32_t nowMs_ = 0;
    bool clockStarted_ = false;

    // Next transition to apply: a system.activate/off taken from the
    // executor, or the fallback after the active activity was deleted.
    TransitionRequest pending_;

    SpscRing<TransitionRequest, 8> mailbox_;
    EngineStats stats_;
};

}  // namespace hubbo
