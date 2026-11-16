#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

#include "hubbo/domain/adapter.h"
#include "hubbo/domain/ids.h"
#include "hubbo/domain/smart_buttons.h"
#include "hubbo/engine/compiled.h"
#include "hubbo/engine/executor.h"
#include "hubbo/engine/hold_ledger.h"
#include "hubbo/engine/run_history.h"
#include "hubbo/engine/snapshot.h"
#include "hubbo/engine/spsc.h"
#include "hubbo/remote/input.h"

// The activity engine: the remote's brain. Owns the active activity, turns
// remote events into queued sequences, and runs activity transitions.
//
// Threading. Everything except submit() and activeActivityIdShared() runs on
// one task (the engine/RF task). Config arrives from another task through the
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
// Held keys are guaranteed at runtime by the hold ledger (hold_ledger.h,
// design section 5), not by config validation:
//   * a Hold-effect command (key_down) executed from a button's binding is
//     recorded with its release command; a release forgets it once it has
//     completed successfully (Accepted, or a Pending result that arrived);
//     a pending release that fails, times out or is cancelled falls back
//     to the adapter's releaseAll();
//   * when the button goes up, every hold still open for it is released
//     once its up sequence has ended, however it ended (completed, failed,
//     cancelled, rejected), or at once if nothing is bound to it; a hold
//     that only executes after its button went up is released at once;
//   * a lost release (radio timeout) is an up like any other, plus a
//     releaseAll() on every adapter;
//   * every transition releases all open holds and calls releaseAll().
//
// Smart buttons (docs/design/smart-buttons.md). For the six smart buttons
// (Light 1/2, Switch 1/2, +, -) a press resolves per button, at its Down:
// the active activity, if it binds any trigger of the button; else the
// slot's press (a slot button's own `button`, or the selected slot's
// `plus`/`minus`) if it has any actions; else the global bindings.
//   * The press a Down picked (its slot and which press) is kept until the
//     Up, so its Tap, Hold and release all come from it whatever changes
//     meanwhile: another slot selected, an activity switch (the release
//     still runs: a moving cover must stop) or a config save (the spans are
//     read from the running config by that slot and press, never kept).
//   * Selection: a Down of a slot button the activity doesn't bind selects
//     that slot (even an empty one). RAM only; Light 1 at boot.
//   * Tap runs `tap`, Hold `hold`, and the Up after a Hold (a timed-out one
//     too) `release`. Down runs nothing.
//   * Step taps: a + or - Tap whose tap actions have an int `step` param
//     (CompiledSmartPress::merges) isn't sent at once. Taps add up and go as
//     one call kSmartStepWindowMs after the last one, with step x taps
//     (clamped to the param's range); no timer while the button is down
//     again. Sent at once (flushed) by a tap the other way, a slot change,
//     an activity change or off, a Hold of + or -, or a config change (with
//     the actions as tapped, never the new config's).
namespace hubbo {

constexpr uint32_t kSmartStepWindowMs = 300;

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

// One phase of a transition (startup of the new activity, shutdown of the
// old one).
enum class PhaseStatus : uint8_t {
    None,      // not part of this transition (or the sequence is empty)
    Queued,    // `actions` queued
    Rejected,  // not queued; `reason` says why (QueueFull)
};
const char *phaseStatusName(PhaseStatus status);  // "none", "queued", "rejected"

struct TransitionPhase {
    PhaseStatus status = PhaseStatus::None;
    ErrorCode reason = ErrorCode::Ok;  // Rejected only
    uint8_t actions = 0;               // Queued only
};

// What an activate/off did (the API's TransitionResult).
//
//   error     Ok, or why nothing happened (UnknownActivity, NoActiveActivity):
//             then switched == false and nothing changed.
//   switched  the active activity changed (keys released, generation bumped).
//             A switched transition is final even if a phase was rejected:
//             the API must not report it as a retryable failure.
struct TransitionResult {
    ErrorCode error = ErrorCode::Ok;
    bool switched = false;
    IdString activeActivityId;    // "" = none
    IdString previousActivityId;  // "" = none
    uint32_t generation = 0;
    TransitionPhase startup;      // activate
    TransitionPhase shutdown;     // off
    uint8_t queuedActions = 0;    // startup.actions + shutdown.actions (kept for existing callers)
};

// What a directly executed command did (the API's ExecuteResult).
struct CommandResult {
    ExecStatus status = ExecStatus::Failed;  // Accepted, Failed or Timeout; never Pending
    ErrorCode error = ErrorCode::Ok;
    uint32_t elapsedMs = 0;
};

// A macro run from the editor (EngineRequest::runMacro), once done.
struct MacroRunResult {
    // Ok (queued on the Macro lane), QueueFull (the lane is full: refused,
    // counted, not recorded), TooManyActions (0 or over the limit).
    ErrorCode error = ErrorCode::Ok;
    uint32_t runId = 0;  // ActivityRun::id (0: not recorded, or no history)
};

// A request from another task (e.g. HTTP) with a reply. The caller owns it,
// fills it, submit()s it, and waits until done() (the engine never blocks
// on it; how the caller waits is its business: vTaskDelay on the board,
// advancing simulated time in hub-sim). Don't touch it again until done().
// Transitions are done on the next poll(); executions when the command's
// result is known (immediately, or after the adapter's Pending result or
// the action timeout).
class EngineRequest {
public:
    enum class Kind : uint8_t { Activate, Off, Execute, RunMacro };

    EngineRequest() = default;
    EngineRequest(const EngineRequest &) = delete;
    EngineRequest &operator=(const EngineRequest &) = delete;

    void activate(std::string_view activityId) {
        kind_ = Kind::Activate;
        activityId_.assign(activityId);  // too long -> "" -> UnknownActivity
    }
    void off() { kind_ = Kind::Off; }
    void execute(const CompiledCommand &command) {
        kind_ = Kind::Execute;
        command_ = command;
    }
    // A macro straight from the editor (ConfigService::compileMacro's
    // steps; the caller keeps them until done()), run as a press of
    // `trigger` would: the Macro lane, origin Macro, recorded with
    // `activityId` ("" for none; any activity, active or not). Done once
    // queued (or refused), not when it ends; an activity switch cancels it.
    void runMacro(const CompiledCommand *steps, size_t count, Trigger trigger, std::string_view activityId) {
        kind_ = Kind::RunMacro;
        steps_ = steps;
        stepCount_ = count;
        trigger_ = trigger;
        activityId_.assign(activityId);
    }

    bool done() const { return done_.load(std::memory_order_acquire); }
    const TransitionResult &transition() const { return transition_; }  // Activate/Off, once done
    const CommandResult &command() const { return result_; }           // Execute, once done
    const MacroRunResult &macro() const { return macro_; }             // RunMacro, once done

private:
    friend class Engine;
    Kind kind_ = Kind::Off;
    IdString activityId_;
    CompiledCommand command_;
    const CompiledCommand *steps_ = nullptr;  // RunMacro
    size_t stepCount_ = 0;
    Trigger trigger_;
    MacroRunResult macro_;
    TransitionResult transition_;
    CommandResult result_;
    uint32_t sequenceId_ = 0;
    uint32_t startedMs_ = 0;
    std::atomic<bool> done_{false};
};

struct EngineStats {
    uint32_t events = 0;
    uint32_t unbound = 0;
    uint32_t dispatched = 0;
    uint32_t forcedReleases = 0;  // holds the ledger released (not the bindings)
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
    // Where startup and shutdown runs are recorded (run_history.h); set
    // once, before the engine runs. Null (the default): not recorded.
    void setRunHistory(RunHistory *history) { history_ = history; }
    // Any task: the recorded runs, newest first. False without a history.
    bool runsShared(ActivityRuns *out) const {
        if (history_ == nullptr) return false;
        history_->read(out);
        return true;
    }
    // Any task: the newest run in brief. False without a history or runs.
    bool lastRunShared(LastRun *out) const { return history_ != nullptr && history_->readLatest(out); }

    // RemoteInput sink (engine task). Event time is taken as "now".
    void onRemoteEvent(const remote::RemoteEvent &event) override;

    // Engine task. Return TransitionResult::error: Ok when it switched (even
    // if its sequence was rejected; see `result`), else UnknownActivity /
    // NoActiveActivity. `result` (optional) gets the details.
    ErrorCode activate(std::string_view activityId, uint32_t nowMs, TransitionResult *result = nullptr);
    ErrorCode off(uint32_t nowMs, TransitionResult *result = nullptr);

    // Boot: make `activityId` active as it was before the reboot, without
    // running its startup, without releaseAll and without bumping the
    // generation; the devices are presumably still on. Reported to the
    // observer like an activation. UnknownActivity (nothing active) if the
    // config doesn't have it.
    ErrorCode restoreActive(std::string_view activityId, uint32_t nowMs);

    // Engine task, every loop iteration: handles submitted requests, delivers
    // adapter completions, resumes waits, applies config swaps.
    void poll(uint32_t nowMs);

    // Any one other task: queue a request with a reply (see EngineRequest).
    // False if the mailbox is full; the request is then untouched.
    bool submit(EngineRequest &request);

    // Engine-task state.
    bool hasActiveActivity() const { return activeIndex_ >= 0; }
    const char *activeActivityId() const { return activeId_.c_str(); }
    // The active id, readable from any task ("" = none).
    std::string activeActivityIdShared() const { return shared_.load(); }
    uint32_t generation() const { return generation_; }
    uint32_t nowMs() const { return nowMs_; }
    // The selected smart-button slot, readable from any task (status).
    SmartSlot selectedSlotShared() const { return static_cast<SmartSlot>(selected_.load(std::memory_order_relaxed)); }

    Executor &executor() { return executor_; }
    const Executor &executor() const { return executor_; }
    const EngineStats &stats() const { return stats_; }
    size_t openHolds() const { return holds_.size(); }  // engine task (tests)

private:
    // The press each smart button's Down picked, until its next Down.
    struct SmartRef {
        bool set = false;
        uint8_t slot = 0;
        SmartPress press = SmartPress::Button;
        const CompiledSmartPress &in(const CompiledConfig &config) const { return config.smartSlots[slot].at(press); }
    };

    void advanceClock(uint32_t nowMs);
    // Notices a new config. A deleted active activity leaves a fallback
    // request in pending_ (or reports inactive if none remain).
    void syncConfig(const Store::Guard &guard);
    // The one place requests become transitions (or rejections).
    ErrorCode apply(const CompiledConfig &config, const TransitionRequest &request,
                    TransitionResult *result = nullptr);
    // Reports a refused transition to the observer and `result`.
    ErrorCode reject(ErrorCode code, const char *activityId, TransitionResult *result);
    void applyPending(const CompiledConfig *config);
    ErrorCode transition(const CompiledConfig &config, int targetIndex, TransitionResult *result = nullptr);
    void handle(EngineRequest &request);
    void finishCommand(const SequenceReport &report);
    // Pump the executor, applying pending and system.activate/off requests.
    void pumpAndApply(const CompiledConfig *config);
    // Coalesced step taps (see above): add one, or send what's pending.
    bool addStepTap(const CompiledConfig &config, const SmartRef &pressed);
    void flushStep();
    // An activity change's teardown (to another, to none, or gone): new
    // generation, queued work cancelled, pending taps sent, holds released.
    void endActivity();
    // The hold ledger (hold_ledger.h).
    void trackHolds(const DispatchRecord &record);
    void resolveRelease(const CompletionRecord &record);
    void resolveReleases(const SequenceReport &report);
    void forceRelease(const OpenHold &hold);
    void releaseHolds(remote::Button button);
    void releaseAllHolds();
    void releaseHold(const OpenHold &hold);

    Store &store_;
    Store::Reader reader_;
    AdapterSet &adapters_;
    Executor executor_;
    EngineObserver *observer_ = nullptr;
    RunHistory *history_ = nullptr;

    int activeIndex_ = -1;
    IdString activeId_;

    // Seqlock copy of activeId_ for other tasks (writer: the engine task).
    class SharedId {
    public:
        void store(std::string_view id);
        std::string load() const;

    private:
        std::atomic<uint32_t> seq_{0};
        std::atomic<char> chars_[limits::kMaxIdLength + 1] = {};
    } shared_;
    void publishActive() { shared_.store(activeId_.view()); }
    uint32_t seenSerial_ = 0;
    uint32_t generation_ = 0;
    uint32_t nowMs_ = 0;
    bool clockStarted_ = false;

    // Next transition to apply: a system.activate/off taken from the
    // executor, or the fallback after the active activity was deleted.
    TransitionRequest pending_;

    SpscRing<EngineRequest *, 8> requests_;

    HoldLedger holds_;
    remote::ButtonSet held_ = 0;  // buttons down, from the events
    remote::ButtonSet holdFired_ = 0;  // buttons down whose Hold fired

    std::atomic<uint8_t> selected_{static_cast<uint8_t>(SmartSlot::Light1)};
    SmartRef pressed_[remote::kButtonCount];
    // Step taps waiting to go as one call (count 0: none). `command` is the
    // first tap's action, copied with its device, so the call goes as
    // tapped whatever a config save changes meanwhile.
    struct StepTaps {
        SmartRef press;  // Plus or Minus
        CompiledCommand command;
        uint16_t count = 0;
        uint32_t dueAtMs = 0;
        remote::Button button() const {
            return press.press == SmartPress::Plus ? remote::Button::Plus : remote::Button::Minus;
        }
    } step_;
    uint32_t upSequence_[remote::kButtonCount] = {};  // the :up sequence per button, until it ends

    // Executions waiting for their sequence to finish.
    static constexpr size_t kMaxPendingCommands = 4;
    EngineRequest *pendingCommands_[kMaxPendingCommands] = {};

    // Sees every executor report first (to complete executions), then
    // forwards to the user's observer.
    struct Relay : ExecutionObserver {
        Engine *engine = nullptr;
        void onActionDispatched(const DispatchRecord &record) override;
        void onActionCompleted(const CompletionRecord &record) override;
        void onSequenceFinished(const SequenceReport &report) override;
    } relay_;
    EngineStats stats_;
};

}  // namespace hubbo
