#include "hubbo/engine/executor.h"

#include "hubbo/domain/ids.h"
#include "hubbo/domain/time.h"

namespace hubbo {

namespace {

// The error a non-accepted result stands for.
ErrorCode failureCode(const ExecResult &result) {
    if (result.status == ExecStatus::Timeout) return ErrorCode::AdapterTimeout;
    return result.error == ErrorCode::Ok ? ErrorCode::AdapterFailed : result.error;
}

}  // namespace

const char *sequenceOriginName(SequenceOrigin origin) {
    switch (origin) {
        case SequenceOrigin::Startup: return "startup";
        case SequenceOrigin::Shutdown: return "shutdown";
        case SequenceOrigin::Binding: return "binding";
        case SequenceOrigin::GlobalBinding: return "global_binding";
        case SequenceOrigin::Api: return "api";
    }
    return "binding";
}

const char *sequenceOutcomeName(SequenceOutcome outcome) {
    switch (outcome) {
        case SequenceOutcome::Completed: return "completed";
        case SequenceOutcome::Failed: return "failed";
        case SequenceOutcome::Cancelled: return "cancelled";
        case SequenceOutcome::Rejected: return "rejected";
    }
    return "failed";
}

Executor::Executor(AdapterSet &adapters, uint32_t actionTimeoutMs)
    : adapters_(adapters), actionTimeoutMs_(actionTimeoutMs) {
    for (size_t i = 0; i < limits::kExecutorActionPool; i++) {
        pool_[i].next = i + 1 < limits::kExecutorActionPool ? static_cast<uint16_t>(i + 1) : kNil;
    }
    freeHead_ = 0;
    freeCount_ = limits::kExecutorActionPool;
}

Executor::Sequence *Executor::reserve(SequenceInfo &info, uint8_t count, uint32_t nowMs) {
    info.id = nextSequenceId_++;
    info.actionCount = count;
    LaneQueue &lane = lanes_[static_cast<size_t>(info.lane)];
    if (lane.count == limits::kExecutorSequencesPerLane || freeCount_ < count) {
        finish(info, SequenceOutcome::Rejected, ErrorCode::QueueFull, 0, nowMs);
        return nullptr;
    }
    Sequence &seq = lane.items[(lane.first + lane.count) % limits::kExecutorSequencesPerLane];
    seq = Sequence{};
    seq.info = info;
    lane.count++;
    stats_.enqueued++;
    return &seq;
}

ErrorCode Executor::enqueue(SequenceInfo info, const CompiledConfig &config, ActionSpan span, uint32_t nowMs,
                            uint32_t *sequenceId) {
    if (info.generation != generation_) {
        return ErrorCode::Cancelled;
    }
    if (span.count == 0) {
        return ErrorCode::Ok;
    }
    if (span.count > limits::kMaxSequenceLength ||
        static_cast<size_t>(span.offset) + span.count > config.actions.size()) {
        return ErrorCode::TooManyActions;
    }
    Sequence *seq = reserve(info, span.count, nowMs);
    if (sequenceId != nullptr) *sequenceId = info.id;
    if (seq == nullptr) return ErrorCode::QueueFull;

    uint16_t tail = kNil;
    const CompiledAction *actions = config.span(span);
    for (uint8_t i = 0; i < span.count; i++) {
        const uint16_t n = takeNode();
        Node &node = pool_[n];
        node.action = actions[i];
        node.target.clear();
        node.providerRef.clear();
        node.next = kNil;
        const bool device = node.action.op == ActionOp::Adapter || node.action.op == ActionOp::Unavailable;
        if (device && node.action.deviceIndex < config.devices.size()) {
            const CompiledDevice &device = config.devices[node.action.deviceIndex];
            node.target.assign(device.id.view());
            node.providerRef.assign(device.providerRef.view());
        } else if (node.action.op == ActionOp::Activate) {
            const auto target = static_cast<size_t>(node.action.params[0].integer);
            if (target < config.activities.size()) {
                node.target.assign(config.activities[target].id.view());
            }
        }
        if (tail == kNil) {
            seq->head = n;
        } else {
            pool_[tail].next = n;
        }
        tail = n;
    }
    return ErrorCode::Ok;
}

ErrorCode Executor::enqueueCommand(SequenceInfo info, const CompiledCommand &command, uint32_t nowMs,
                                   uint32_t *sequenceId) {
    if (info.generation != generation_) return ErrorCode::Cancelled;
    Sequence *seq = reserve(info, 1, nowMs);
    if (sequenceId != nullptr) *sequenceId = info.id;
    if (seq == nullptr) return ErrorCode::QueueFull;
    const uint16_t n = takeNode();
    Node &node = pool_[n];
    node.action = command.action;
    node.target = command.deviceId;
    node.providerRef = command.providerRef;
    node.next = kNil;
    seq->head = n;
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
            stats_.timeouts++;
            finish(f.info, SequenceOutcome::Failed, ErrorCode::AdapterTimeout, f.index, nowMs);
        }
    }
}

void Executor::runLane(LaneQueue &lane, uint32_t nowMs) {
    while (lane.count > 0) {
        Sequence &seq = lane.front();

        if (seq.info.generation != generation_) {
            finishFront(lane, SequenceOutcome::Cancelled, ErrorCode::Cancelled, seq.nextIndex, nowMs);
            continue;
        }
        if (seq.state == State::Waiting) {
            if (!timeReached(nowMs, seq.untilMs)) return;
            seq.state = State::Ready;
        } else if (seq.state == State::Pending) {
            if (!timeReached(nowMs, seq.untilMs)) return;
            stats_.timeouts++;
            finishFront(lane, SequenceOutcome::Failed, ErrorCode::AdapterTimeout,
                        static_cast<uint8_t>(seq.nextIndex - 1), nowMs);
            continue;
        }

        if (seq.head == kNil) {
            finishFront(lane, SequenceOutcome::Completed, ErrorCode::Ok, seq.nextIndex, nowMs);
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
        const uint8_t index = seq.nextIndex++;
        const bool last = seq.head == kNil;
        const CompiledAction &action = node.action;

        if (action.op == ActionOp::Unavailable) {
            if (observer_ != nullptr) {
                const IntegrationAdapter *adapter = adapters_.at(action.adapterIndex);
                observer_->onActionDispatched({seq.info, index, action,
                                               adapter != nullptr ? adapter->integrationId() : "?",
                                               node.target.c_str(), node.providerRef.c_str(),
                                               ExecResult::failed(ErrorCode::IntegrationDisabled), nowMs});
            }
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
            finishFront(lane, SequenceOutcome::Completed, ErrorCode::Ok, seq.nextIndex, nowMs);
            return;
        }

        IntegrationAdapter *adapter = adapters_.at(action.adapterIndex);
        if (adapter == nullptr || action.command == nullptr) {
            finishFront(lane, SequenceOutcome::Failed, ErrorCode::Internal, index, nowMs);
            continue;
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
                                  /*answerAtStart=*/seq.info.origin == SequenceOrigin::Api};
        const ExecResult result = adapter->execute(request);
        if (observer_ != nullptr) {
            observer_->onActionDispatched({seq.info, index, action, adapter->integrationId(), node.target.c_str(),
                                           node.providerRef.c_str(), result, nowMs});
        }

        if (result.status == ExecStatus::Accepted) continue;
        if (result.status != ExecStatus::Pending) {
            if (result.status == ExecStatus::Timeout) stats_.timeouts++;
            finishFront(lane, SequenceOutcome::Failed, failureCode(result), index, nowMs);
            continue;
        }

        // A command that stays Pending longer (a timed press, a wait for a
        // connection) says so; the adapter finishes it first.
        const uint32_t timeoutMs = action.command->timeoutMs != 0 ? action.command->timeoutMs : actionTimeoutMs_;
        const PendingAction pending{adapter, action.command, node.target};
        if (last) {
            if (InFlight *f = freeInFlight()) {
                f->used = true;
                f->ticket = ticket;
                f->info = seq.info;
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
    const bool ok = result.status == ExecStatus::Accepted;

    for (auto &lane : lanes_) {
        if (lane.count == 0) continue;
        Sequence &seq = lane.front();
        if (seq.state != State::Pending || seq.ticket != ticket) continue;
        reportCompletion(seq.info, static_cast<uint8_t>(seq.nextIndex - 1), lane.pending, result, nowMs);
        seq.state = State::Ready;  // done: nothing left to cancel
        seq.ticket = 0;
        if (!ok) {
            if (result.status == ExecStatus::Timeout) stats_.timeouts++;
            finishFront(lane, SequenceOutcome::Failed, failureCode(result), static_cast<uint8_t>(seq.nextIndex - 1),
                        nowMs);
        }
        return;
    }
    for (auto &f : inFlight_) {
        if (!f.used || f.ticket != ticket) continue;
        f.used = false;
        reportCompletion(f.info, f.index, f.pending, result, nowMs);
        if (ok) {
            finish(f.info, SequenceOutcome::Completed, ErrorCode::Ok, f.info.actionCount, nowMs);
        } else {
            if (result.status == ExecStatus::Timeout) stats_.timeouts++;
            finish(f.info, SequenceOutcome::Failed, failureCode(result), f.index, nowMs);
        }
        return;
    }
    // Unknown or stale ticket: ignored by design.
}

void Executor::reportCompletion(const SequenceInfo &info, uint8_t index, const PendingAction &pending,
                                const ExecResult &result, uint32_t nowMs) {
    if (observer_ == nullptr) return;
    observer_->onActionCompleted(
        {info, index, pending.adapter->integrationId(), pending.command, pending.target.c_str(), result, nowMs});
}

void Executor::cancelPending(const PendingAction &pending, Ticket ticket, uint32_t nowMs) {
    pending.adapter->cancel(ticket, nowMs);
}

void Executor::cancelAll(uint32_t newGeneration, uint32_t nowMs) {
    generation_ = newGeneration;
    transition_ = {};
    for (auto &lane : lanes_) {
        while (lane.count > 0) {
            finishFront(lane, SequenceOutcome::Cancelled, ErrorCode::Cancelled, lane.front().nextIndex, nowMs);
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

void Executor::finishFront(LaneQueue &lane, SequenceOutcome outcome, ErrorCode error, uint8_t index,
                           uint32_t nowMs) {
    const Sequence &seq = lane.front();
    // Cancelled, or past its deadline, while its adapter still works on it.
    if (seq.state == State::Pending) cancelPending(lane.pending, seq.ticket, nowMs);
    const SequenceInfo info = seq.info;  // popFront() resets the slot
    popFront(lane);
    finish(info, outcome, error, index, nowMs);
}

void Executor::popFront(LaneQueue &lane) {
    Sequence &seq = lane.front();
    uint16_t n = seq.head;
    while (n != kNil) {
        const uint16_t next = pool_[n].next;
        freeNode(n);
        n = next;
    }
    seq = Sequence{};
    lane.first = (lane.first + 1) % limits::kExecutorSequencesPerLane;
    lane.count--;
}

void Executor::finish(const SequenceInfo &info, SequenceOutcome outcome, ErrorCode error, uint8_t index,
                      uint32_t nowMs) {
    switch (outcome) {
        case SequenceOutcome::Completed: stats_.completed++; break;
        case SequenceOutcome::Failed: stats_.failed++; break;
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
