#include "hubbo/engine/executor.h"

#include <iterator>

#include "hubbo/domain/ids.h"
#include "hubbo/domain/time.h"

namespace hubbo {

const char *sequenceOriginName(SequenceOrigin origin) {
    switch (origin) {
        case SequenceOrigin::Startup: return "startup";
        case SequenceOrigin::Shutdown: return "shutdown";
        case SequenceOrigin::Binding: return "binding";
        case SequenceOrigin::GlobalBinding: return "global_binding";
        case SequenceOrigin::Api: return "api";
        case SequenceOrigin::SmartButton: return "smart_button";
        case SequenceOrigin::Macro: return "macro";
    }
    return "binding";
}

const char *sequenceOutcomeName(SequenceOutcome outcome) {
    switch (outcome) {
        case SequenceOutcome::Completed: return "completed";
        case SequenceOutcome::CompletedWithErrors: return "completed_with_errors";
        case SequenceOutcome::Cancelled: return "cancelled";
        case SequenceOutcome::Rejected: return "rejected";
    }
    return "completed";
}

Executor::Executor(AdapterSet &adapters, uint32_t actionTimeoutMs)
    : adapters_(adapters), actionTimeoutMs_(actionTimeoutMs) {
    for (size_t i = 0; i < limits::kExecutorActionPool; i++) {
        pool_[i].next = i + 1 < limits::kExecutorActionPool ? static_cast<uint16_t>(i + 1) : kNil;
    }
    freeHead_ = 0;
    freeCount_ = limits::kExecutorActionPool;
    const auto use = [this](Lane lane, Sequence *items, size_t capacity, size_t nodeBudget) {
        LaneQueue &q = lanes_[static_cast<size_t>(lane)];
        q.items = items;
        q.capacity = capacity;
        q.nodeBudget = nodeBudget;
    };
    use(Lane::Transition, transitionItems_, std::size(transitionItems_), limits::kExecutorActionPool);
    use(Lane::Input, inputItems_, std::size(inputItems_), limits::kExecutorActionPool);
    use(Lane::Macro, macroItems_, std::size(macroItems_), limits::kExecutorMacroNodes);
}

Executor::Sequence *Executor::reserve(SequenceInfo &info, uint8_t count, uint32_t nowMs) {
    info.id = nextSequenceId_++;
    info.actionCount = count;
    LaneQueue &lane = lanes_[static_cast<size_t>(info.lane)];
    if (lane.count == lane.capacity || lane.nodes + count > lane.nodeBudget || freeCount_ < count) {
        finish(info, SequenceOutcome::Rejected, ErrorCode::QueueFull, 0, nowMs);
        return nullptr;
    }
    Sequence &seq = lane.items[(lane.first + lane.count) % lane.capacity];
    seq = Sequence{};
    seq.info = info;
    lane.count++;
    lane.nodes += count;
    stats_.enqueued++;
    return &seq;
}

template <typename Fill>
void Executor::link(Sequence &seq, size_t count, Fill fill) {
    uint16_t tail = kNil;
    for (size_t i = 0; i < count; i++) {
        const uint16_t n = takeNode();
        Node &node = pool_[n];
        node.target.clear();
        node.providerRef.clear();
        node.next = kNil;
        fill(i, &node);
        if (tail == kNil) {
            seq.head = n;
        } else {
            pool_[tail].next = n;
        }
        tail = n;
    }
}

ErrorCode Executor::enqueue(SequenceInfo info, const CompiledConfig &config, ActionSpan span, uint32_t nowMs,
                            uint32_t *sequenceId) {
    if (info.generation != generation_) {
        return ErrorCode::Cancelled;
    }
    if (span.count == 0) {
        return ErrorCode::Ok;
    }
    if (!config.fits(span)) return ErrorCode::TooManyActions;
    Sequence *seq = reserve(info, span.count, nowMs);
    if (sequenceId != nullptr) *sequenceId = info.id;
    if (seq == nullptr) return ErrorCode::QueueFull;
    const CompiledAction *actions = config.span(span);
    link(*seq, span.count, [&](size_t i, Node *node) {
        node->action = actions[i];
        config.targetOf(node->action, &node->target, &node->providerRef);
    });
    return ErrorCode::Ok;
}

ErrorCode Executor::enqueueSteps(SequenceInfo info, const CompiledCommand *steps, size_t count, uint32_t nowMs,
                                 uint32_t *sequenceId) {
    if (info.generation != generation_) return ErrorCode::Cancelled;
    if (count == 0 || count > limits::kMaxSequenceLength) return ErrorCode::TooManyActions;
    Sequence *seq = reserve(info, static_cast<uint8_t>(count), nowMs);
    if (sequenceId != nullptr) *sequenceId = info.id;
    if (seq == nullptr) return ErrorCode::QueueFull;
    link(*seq, count, [&](size_t i, Node *node) {
        node->action = steps[i].action;
        node->target = steps[i].deviceId;
        node->providerRef = steps[i].providerRef;
    });
    return ErrorCode::Ok;
}

void Executor::pump(uint32_t nowMs) {
    for (auto &lane : lanes_) {
        runLane(lane, nowMs);
        if (transition_) return;
    }
    for (auto &f : inFlight_) {
        if (f.used && timeReached(nowMs, f.deadlineMs)) {
            f.used = false;
            cancelPending(f.pending, f.ticket, nowMs);
            reportCompletion(f.info, f.index, f.pending, ExecResult::timeout(), nowMs);
            stepFailed(f.errors, f.index, ExecResult::timeout());
            finishDone(f.info, f.errors, nowMs);
        }
    }
}

void Executor::runLane(LaneQueue &lane, uint32_t nowMs) {
    while (lane.count > 0) {
        Sequence &seq = lane.front();

        if (seq.info.generation != generation_) {
            cancelFront(lane, nowMs);
            continue;
        }
        if (seq.state == State::Waiting) {
            if (!timeReached(nowMs, seq.untilMs)) return;
            seq.state = State::Ready;
        } else if (seq.state == State::Pending) {
            if (!timeReached(nowMs, seq.untilMs)) return;
            // Timed out: stop it, and go on with the next step.
            const auto index = static_cast<uint8_t>(seq.nextIndex - 1);
            cancelPending(lane.pending, seq.ticket, nowMs);
            reportCompletion(seq.info, index, lane.pending, ExecResult::timeout(), nowMs);
            stepFailed(seq.errors, index, ExecResult::timeout());
            seq.state = State::Ready;
            seq.ticket = 0;
        }

        if (seq.head == kNil) {
            doneFront(lane, nowMs);
            continue;
        }

        // Take the next action off the sequence. The node goes back to the
        // pool when this iteration ends; nothing can take it before then
        // (only enqueue() takes nodes, and it isn't called during a pump).
        struct Release {
            Executor &self;
            uint16_t n;
            ~Release() { self.freeNode(n); }
        } release{*this, seq.head};
        const Node &node = pool_[release.n];
        seq.head = node.next;
        lane.nodes--;
        const uint8_t index = seq.nextIndex++;
        const bool last = seq.head == kNil;
        const CompiledAction &action = node.action;

        IntegrationAdapter *adapter = adapters_.at(action.adapterIndex);
        // A step that can't reach its adapter is a failed step like any other.
        const bool unreachable = action.op == ActionOp::Adapter && (adapter == nullptr || action.command == nullptr);
        if (action.op == ActionOp::Unavailable || unreachable) {
            const ExecResult result =
                ExecResult::failed(unreachable ? ErrorCode::Internal : ErrorCode::IntegrationDisabled);
            if (observer_ != nullptr) {
                observer_->onActionDispatched({seq.info, index, action,
                                               adapter != nullptr ? adapter->integrationId() : "?",
                                               node.target.c_str(), node.providerRef.c_str(), result, nowMs});
            }
            stepFailed(seq.errors, index, result);
            continue;
        }
        if (action.op != ActionOp::Adapter) {
            if (observer_ != nullptr) {
                observer_->onActionDispatched({seq.info, index, action, kSystemIntegrationId, node.target.c_str(), "",
                                               ExecResult::accepted(), nowMs});
            }
            if (action.op == ActionOp::Wait) {
                const auto duration = static_cast<uint32_t>(action.params[0].integer);
                if (duration > 0) {
                    seq.state = State::Waiting;
                    seq.untilMs = nowMs + duration;
                }
                continue;
            }
            // Activate/Off: always last in its sequence (compiler rule).
            transition_ = action.op == ActionOp::Activate ? TransitionRequest::activate(node.target.view())
                                                          : TransitionRequest::off();
            doneFront(lane, nowMs);
            return;
        }

        const Ticket ticket = nextTicket();
        const ExecRequest request{action.command,
                                  action.commandIndex,
                                  action.params,
                                  action.paramsPresent,
                                  node.target.c_str(),
                                  node.providerRef.c_str(),
                                  ticket,
                                  nowMs,
                                  /*answerAtStart=*/seq.info.origin == SequenceOrigin::Api,
                                  action.call,
                                  /*checkState=*/seq.info.lane == Lane::Transition};
        const ExecResult result = adapter->execute(request);
        if (observer_ != nullptr) {
            observer_->onActionDispatched({seq.info, index, action, adapter->integrationId(), node.target.c_str(),
                                           node.providerRef.c_str(), result, nowMs});
        }

        if (succeeded(result.status)) continue;
        if (result.status != ExecStatus::Pending) {
            stepFailed(seq.errors, index, result);
            continue;
        }

        // A command that stays Pending longer (a timed press, a wait for a
        // connection) says so; the adapter finishes it first.
        const uint32_t timeoutMs = action.command->timeoutMs != 0 ? action.command->timeoutMs : actionTimeoutMs_;
        const PendingAction pending{adapter, action.command, node.target, nowMs};
        if (last) {
            if (InFlight *f = freeInFlight()) {
                f->used = true;
                f->ticket = ticket;
                f->info = seq.info;
                f->errors = seq.errors;
                f->index = index;
                f->deadlineMs = nowMs + timeoutMs;
                f->pending = pending;
                popFront(lane);  // reported when the result arrives
                continue;
            }
        }
        // More to do (or nowhere to track it): park the lane until the result.
        seq.state = State::Pending;
        seq.ticket = ticket;
        seq.untilMs = nowMs + timeoutMs;
        lane.pending = pending;
        return;
    }
}

void Executor::complete(Ticket ticket, ExecResult result, uint32_t nowMs) {
    if (ticket == 0 || result.status == ExecStatus::Pending) return;

    for (auto &lane : lanes_) {
        if (lane.count == 0) continue;
        Sequence &seq = lane.front();
        if (seq.state != State::Pending || seq.ticket != ticket) continue;
        const auto index = static_cast<uint8_t>(seq.nextIndex - 1);
        reportCompletion(seq.info, index, lane.pending, result, nowMs);
        seq.state = State::Ready;  // done: nothing left to cancel; the next pump goes on
        seq.ticket = 0;
        stepFailed(seq.errors, index, result);
        return;
    }
    for (auto &f : inFlight_) {
        if (!f.used || f.ticket != ticket) continue;
        f.used = false;
        reportCompletion(f.info, f.index, f.pending, result, nowMs);
        stepFailed(f.errors, f.index, result);
        finishDone(f.info, f.errors, nowMs);
        return;
    }
    // Unknown or stale ticket: ignored by design.
}

void Executor::reportCompletion(const SequenceInfo &info, uint8_t index, const PendingAction &pending,
                                const ExecResult &result, uint32_t nowMs) {
    if (observer_ == nullptr) return;
    observer_->onActionCompleted(
        {info, index, pending.adapter->integrationId(), pending.command, pending.target.c_str(), result, nowMs,
         pending.startedMs});
}

void Executor::cancelPending(const PendingAction &pending, Ticket ticket, uint32_t nowMs) {
    pending.adapter->cancel(ticket, nowMs);
}

void Executor::cancelAll(uint32_t newGeneration, uint32_t nowMs) {
    generation_ = newGeneration;
    transition_ = {};
    for (auto &lane : lanes_) {
        while (lane.count > 0) {
            cancelFront(lane, nowMs);
        }
    }
    for (auto &f : inFlight_) {
        if (!f.used) continue;
        f.used = false;
        cancelPending(f.pending, f.ticket, nowMs);
        finish(f.info, SequenceOutcome::Cancelled, ErrorCode::Cancelled, f.index, nowMs);
    }
}

bool Executor::nextDeadline(uint32_t *atMs) const {
    bool any = false;
    uint32_t best = 0;
    auto consider = [&](uint32_t t) {
        if (!any || timeAfter(best, t)) best = t;
        any = true;
    };
    for (const auto &lane : lanes_) {
        if (lane.count == 0) continue;
        const Sequence &seq = lane.front();
        if (seq.state != State::Ready) consider(seq.untilMs);
    }
    for (const auto &f : inFlight_) {
        if (f.used) consider(f.deadlineMs);
    }
    if (any && atMs != nullptr) *atMs = best;
    return any;
}

bool Executor::idle() const {
    for (const auto &lane : lanes_) {
        if (lane.count != 0) return false;
    }
    return inFlight() == 0;
}

size_t Executor::inFlight() const {
    size_t n = 0;
    for (const auto &f : inFlight_) n += f.used ? 1 : 0;
    return n;
}

void Executor::cancelFront(LaneQueue &lane, uint32_t nowMs) {
    const Sequence &seq = lane.front();
    if (seq.state == State::Pending) cancelPending(lane.pending, seq.ticket, nowMs);  // its adapter still works on it
    const SequenceInfo info = seq.info;  // popFront() resets the slot
    const uint8_t next = seq.nextIndex;
    popFront(lane);
    finish(info, SequenceOutcome::Cancelled, ErrorCode::Cancelled, next, nowMs);
}

void Executor::doneFront(LaneQueue &lane, uint32_t nowMs) {
    const SequenceInfo info = lane.front().info;  // popFront() resets the slot
    const FirstError errors = lane.front().errors;
    popFront(lane);
    finishDone(info, errors, nowMs);
}

void Executor::finishDone(const SequenceInfo &info, const FirstError &errors, uint32_t nowMs) {
    if (errors.code == ErrorCode::Ok) {
        finish(info, SequenceOutcome::Completed, ErrorCode::Ok, info.actionCount, nowMs);
    } else {
        finish(info, SequenceOutcome::CompletedWithErrors, errors.code, errors.index, nowMs);
    }
}

void Executor::stepFailed(FirstError &errors, uint8_t index, const ExecResult &result) {
    if (succeeded(result.status)) return;
    if (result.status == ExecStatus::Timeout) stats_.timeouts++;
    errors.note(index, failureCode(result));
}

void Executor::popFront(LaneQueue &lane) {
    Sequence &seq = lane.front();
    uint16_t n = seq.head;
    while (n != kNil) {
        const uint16_t next = pool_[n].next;
        freeNode(n);
        lane.nodes--;
        n = next;
    }
    seq = Sequence{};
    lane.first = (lane.first + 1) % lane.capacity;
    lane.count--;
}

void Executor::finish(const SequenceInfo &info, SequenceOutcome outcome, ErrorCode error, uint8_t index,
                      uint32_t nowMs) {
    switch (outcome) {
        case SequenceOutcome::Completed: stats_.completed++; break;
        case SequenceOutcome::CompletedWithErrors: stats_.withErrors++; break;
        case SequenceOutcome::Cancelled: stats_.cancelled++; break;
        case SequenceOutcome::Rejected: stats_.rejected++; break;
    }
    if (observer_ == nullptr) return;
    SequenceReport r;
    r.info = info;
    r.outcome = outcome;
    r.error = error;
    r.actionIndex = index;
    r.atMs = nowMs;
    observer_->onSequenceFinished(r);
}

uint16_t Executor::takeNode() {
    const uint16_t n = freeHead_;
    freeHead_ = pool_[n].next;
    freeCount_--;
    const auto used = static_cast<uint16_t>(limits::kExecutorActionPool - freeCount_);
    if (used > stats_.nodesHighWater) stats_.nodesHighWater = used;
    return n;
}

void Executor::freeNode(uint16_t index) {
    pool_[index].action.call.reset();  // a free node keeps nothing alive
    pool_[index].next = freeHead_;
    freeHead_ = index;
    freeCount_++;
}

Ticket Executor::nextTicket() {
    Ticket t = nextTicket_++;
    if (t == 0) t = nextTicket_++;
    return t;
}

Executor::InFlight *Executor::freeInFlight() {
    for (auto &f : inFlight_) {
        if (!f.used) return &f;
    }
    return nullptr;
}

}  // namespace hubbo
