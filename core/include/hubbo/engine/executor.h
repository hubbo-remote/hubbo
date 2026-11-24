#pragma once

#include <cstddef>
#include <cstdint>

#include "hubbo/domain/adapter.h"
#include "hubbo/domain/ids.h"
#include "hubbo/domain/limits.h"
#include "hubbo/domain/trigger.h"
#include "hubbo/engine/compiled.h"

// Ordered, non-blocking action sequence executor with bounded, preallocated
// storage. Runs on the engine task only (single-threaded by design).
//
// Lanes. Sequences queue FIFO in one of two lanes that progress
// independently, so a 5 s wait in a startup sequence or a macro never delays
// a button:
//   Transition  startup/shutdown sequences
//   Input       one-action button bindings (and API-triggered sequences)
//   Macro       button bindings marked as macros
//               (the running one plus kExecutorMacroSequences queued;
//               docs/design/macros.md)
// Within a lane, sequences run one after another; within a sequence, actions
// run strictly in order.
//
// Waits. system.wait parks its sequence until nowMs + duration; pump() resumes
// it. Nothing sleeps. Time comparisons are wrap-safe.
//
// Adapter results. Accepted -> next action. Failed/Timeout -> the step's
// error is recorded and the next action starts at once: a step never stops
// its sequence (docs/design/activity-runs.md), which ends CompletedWithErrors
// (no retries). Pending -> if more actions follow, the sequence (and its
// lane) waits for complete() or the action timeout (reported as the step's
// Timeout completion); if it was the last action, the lane moves on and the
// result is tracked in a small in-flight table, reported when it arrives or
// times out.
//
// Generation guard. Every sequence carries the engine's transition
// generation. cancelAll(newGeneration) drops every queued/parked sequence and
// forgets in-flight results; any sequence whose generation is not current is
// dropped before each step and refused at enqueue, so a stale startup can
// never resume after a switch. Late completions for forgotten tickets are
// ignored.
//
// Memory. kExecutorActionPool action nodes shared by all lanes,
// kExecutorSequencesPerLane sequences per lane, kExecutorInFlight tracked
// results. Each node holds a copy of the compiled action plus the target's
// device id and provider ref, so queued work never points into a compiled
// config that may be swapped and freed.
//
// Overflow. enqueue() is all-or-nothing: if the lane, its node budget or the
// node pool can't take the whole sequence it is rejected with QueueFull,
// counted, and reported (outcome Rejected). The Macro lane's budget is
// kExecutorMacroNodes, so long macros never starve presses of nodes. Nothing already queued is dropped. The engine
// adds held-key safety on top (see engine.h).
namespace hubbo {

enum class Lane : uint8_t { Transition, Input, Macro, Count };
// SmartButton: a smart-button slot's press (docs/design/smart-buttons.md).
// Macro: a binding (activity or global) marked as a macro, on the Macro
// lane (docs/design/macros.md).
enum class SequenceOrigin : uint8_t { Startup, Shutdown, Binding, GlobalBinding, Api, SmartButton, Macro };
// Completed: every step ok. CompletedWithErrors: it reached the end and one
// or more steps failed or timed out.
enum class SequenceOutcome : uint8_t { Completed, CompletedWithErrors, Cancelled, Rejected };

const char *sequenceOriginName(SequenceOrigin origin);
const char *sequenceOutcomeName(SequenceOutcome outcome);

constexpr size_t kLaneCount = static_cast<size_t>(Lane::Count);

struct SequenceInfo {
    uint32_t id = 0;  // assigned by enqueue()
    Lane lane = Lane::Input;
    SequenceOrigin origin = SequenceOrigin::Binding;
    Trigger trigger;  // Binding/GlobalBinding/SmartButton/Macro only
    IdString activityId;  // owning activity, if any
    uint32_t generation = 0;
    uint8_t actionCount = 0;
};

struct SequenceReport {
    SequenceInfo info;
    SequenceOutcome outcome = SequenceOutcome::Completed;
    ErrorCode error = ErrorCode::Ok;
    // The first failed step (CompletedWithErrors, with `error`) or the next
    // unrun one (Cancelled).
    uint8_t actionIndex = 0;
    uint32_t atMs = 0;
};

// What the executor did with one action, for logging, diagnostics and the
// simulator's command log.
struct DispatchRecord {
    const SequenceInfo &sequence;
    uint8_t index;               // position in the sequence
    const CompiledAction &action;
    const char *integrationId;   // "system" for built-ins
    const char *target;          // device id, activity id for system.activate, else ""
    const char *providerRef;     // the device's provider ref, else ""
    ExecResult result;
    uint32_t atMs;
};

// A Pending action's result, when its adapter reports it (a timed press
// released, a wait for a connection ended), or a Timeout when it misses its
// deadline. An Accepted result may still carry an error (wait_connected
// timing out), logged here.
struct CompletionRecord {
    const SequenceInfo &sequence;
    uint8_t index;               // position in the sequence
    const char *integrationId;
    const CommandSpec *command;
    const char *target;          // device id
    ExecResult result;
    uint32_t atMs;
    uint32_t startedMs;          // when it was dispatched
};

class ExecutionObserver {
public:
    virtual ~ExecutionObserver() = default;
    virtual void onActionDispatched(const DispatchRecord &record) { (void)record; }
    virtual void onActionCompleted(const CompletionRecord &record) { (void)record; }
    virtual void onSequenceFinished(const SequenceReport &report) { (void)report; }
};

// A request to switch activity or turn off. system.activate/off actions
// produce one (the executor records it and stops pumping; the engine takes it
// and applies it), and the engine uses the same type for its own requests.
struct TransitionRequest {
    enum class Kind : uint8_t { None, Activate, Off };
    Kind kind = Kind::None;
    IdString activityId;  // Activate only

    explicit operator bool() const { return kind != Kind::None; }
    static TransitionRequest off() { return {Kind::Off, {}}; }
    static TransitionRequest activate(std::string_view id) {
        TransitionRequest r{Kind::Activate, {}};
        if (!r.activityId.assign(id)) r.kind = Kind::None;  // too long to be an id
        return r;
    }
};

struct ExecutorStats {
    uint32_t enqueued = 0;
    uint32_t completed = 0;
    uint32_t withErrors = 0;  // CompletedWithErrors
    uint32_t cancelled = 0;
    uint32_t rejected = 0;   // overflow
    uint32_t timeouts = 0;
    uint16_t nodesHighWater = 0;
};

class Executor : public CompletionSink {
public:
    explicit Executor(AdapterSet &adapters, uint32_t actionTimeoutMs = limits::kDefaultActionTimeoutMs);
    Executor(const Executor &) = delete;  // its lanes point into it
    Executor &operator=(const Executor &) = delete;

    void setObserver(ExecutionObserver *observer) { observer_ = observer; }

    // Queues config.actions[span] as one sequence (`info.id` is assigned).
    // Does not run anything; call pump(). Returns Ok, QueueFull (rejected
    // whole), Cancelled (stale generation) or TooManyActions. An empty span
    // is a no-op (Ok).
    ErrorCode enqueue(SequenceInfo info, const CompiledConfig &config, ActionSpan span, uint32_t nowMs,
                      uint32_t *sequenceId = nullptr);

    // Queues one self-contained command (API execution) as a sequence.
    ErrorCode enqueueCommand(SequenceInfo info, const CompiledCommand &command, uint32_t nowMs,
                             uint32_t *sequenceId = nullptr) {
        return enqueueSteps(info, &command, 1, nowMs, sequenceId);
    }
    // Queues `count` self-contained commands (a macro run from the editor)
    // as one sequence: QueueFull, TooManyActions (0 or over the limit), Ok.
    ErrorCode enqueueSteps(SequenceInfo info, const CompiledCommand *steps, size_t count, uint32_t nowMs,
                           uint32_t *sequenceId = nullptr);

    // Runs everything that is due. Bounded: each call executes at most the
    // queued actions once. Stops early at a system.activate/off so the engine
    // can apply it (takeTransition()) before anything else runs.
    void pump(uint32_t nowMs);

    // CompletionSink: result of a Pending execution.
    void complete(Ticket ticket, ExecResult result, uint32_t nowMs) override;

    // Drops all queued/parked sequences and in-flight tracking (reported
    // Cancelled) and makes `newGeneration` current.
    void cancelAll(uint32_t newGeneration, uint32_t nowMs);

    uint32_t generation() const { return generation_; }
    // The system.activate/off reached by the last pump(), if any; clears it.
    TransitionRequest takeTransition() {
        const TransitionRequest r = transition_;
        transition_ = {};
        return r;
    }

    // True and the earliest time something is due, if anything is parked.
    bool nextDeadline(uint32_t *atMs) const;

    bool idle() const;
    size_t freeNodes() const { return freeCount_; }
    size_t inFlight() const;
    const ExecutorStats &stats() const { return stats_; }

private:
    static constexpr uint16_t kNil = 0xFFFF;

    enum class State : uint8_t { Ready, Waiting, Pending };

    struct Node {
        CompiledAction action;
        IdString target;  // device id (Adapter) or activity id (Activate)
        FixedString<limits::kMaxProviderRefLength> providerRef;
        uint16_t next = kNil;
    };

    // The action a Pending result belongs to, for onActionCompleted.
    // (and, to cancel it, its adapter).
    struct PendingAction {
        IntegrationAdapter *adapter = nullptr;
        const CommandSpec *command = nullptr;
        std::shared_ptr<const void> catalog;  // keeps `command` (kept until the slot is reused)
        IdString target;
        uint32_t startedMs = 0;  // dispatched at
    };

    // A sequence's first failed step, for its report.
    struct FirstError {
        ErrorCode code = ErrorCode::Ok;
        uint8_t index = 0;
        void note(uint8_t at, ErrorCode error) {
            if (code != ErrorCode::Ok) return;
            code = error;
            index = at;
        }
    };

    struct Sequence {
        SequenceInfo info;
        FirstError errors;
        uint16_t head = kNil;
        uint8_t nextIndex = 0;
        State state = State::Ready;
        uint32_t untilMs = 0;  // Waiting: resume time; Pending: deadline
        Ticket ticket = 0;
    };

    // A ring over storage the executor owns, sized per lane.
    struct LaneQueue {
        Sequence *items = nullptr;
        size_t capacity = 0;
        size_t nodeBudget = 0;  // pool nodes its sequences may hold together
        size_t nodes = 0;       // held now
        size_t first = 0;
        size_t count = 0;
        PendingAction pending;  // the front sequence's, while it is Pending
        Sequence &front() { return items[first]; }
        const Sequence &front() const { return items[first]; }
    };

    struct InFlight {
        bool used = false;
        Ticket ticket = 0;
        SequenceInfo info;
        FirstError errors;  // its earlier steps'
        uint8_t index = 0;
        uint32_t deadlineMs = 0;
        PendingAction pending;
    };

    // Reserves a sequence slot and `count` nodes, or reports Rejected.
    Sequence *reserve(SequenceInfo &info, uint8_t count, uint32_t nowMs);
    // Queues `count` actions on `seq`, step i from fill(i, &node) (its
    // action and target).
    template <typename Fill>
    void link(Sequence &seq, size_t count, Fill fill);
    void runLane(LaneQueue &lane, uint32_t nowMs);
    // The front sequence, cancelled (the rest of its steps never run).
    void cancelFront(LaneQueue &lane, uint32_t nowMs);
    // The front sequence, at its end.
    void doneFront(LaneQueue &lane, uint32_t nowMs);
    // Reached its end: Completed, or CompletedWithErrors with the first error.
    void finishDone(const SequenceInfo &info, const FirstError &errors, uint32_t nowMs);
    // A step's result: a Failed or Timeout one is counted and noted.
    void stepFailed(FirstError &errors, uint8_t index, const ExecResult &result);
    void popFront(LaneQueue &lane);
    // Counts the outcome and tells the observer. Every finished sequence goes
    // through here exactly once.
    void finish(const SequenceInfo &info, SequenceOutcome outcome, ErrorCode error, uint8_t index, uint32_t nowMs);
    uint16_t takeNode();
    void freeNode(uint16_t index);
    Ticket nextTicket();
    InFlight *freeInFlight();
    void cancelPending(const PendingAction &pending, Ticket ticket, uint32_t nowMs);
    void reportCompletion(const SequenceInfo &info, uint8_t index, const PendingAction &pending,
                          const ExecResult &result, uint32_t nowMs);

    AdapterSet &adapters_;
    ExecutionObserver *observer_ = nullptr;
    uint32_t actionTimeoutMs_;

    Node pool_[limits::kExecutorActionPool];
    uint16_t freeHead_ = kNil;
    size_t freeCount_ = 0;
    LaneQueue lanes_[kLaneCount];
    Sequence transitionItems_[limits::kExecutorSequencesPerLane];
    Sequence inputItems_[limits::kExecutorSequencesPerLane];
    Sequence macroItems_[limits::kExecutorMacroSequences + 1];  // the running one too
    InFlight inFlight_[limits::kExecutorInFlight];

    uint32_t generation_ = 0;
    uint32_t nextSequenceId_ = 1;
    Ticket nextTicket_ = 1;
    TransitionRequest transition_;
    ExecutorStats stats_;
};

}  // namespace hubbo
