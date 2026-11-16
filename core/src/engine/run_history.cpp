#include "hubbo/engine/run_history.h"

#include <algorithm>

namespace hubbo {

const char *stepResultName(StepResult result) {
    switch (result) {
        case StepResult::Pending: return "pending";
        case StepResult::Ok: return "ok";
        case StepResult::Failed: return "failed";
        case StepResult::TimedOut: return "timed_out";
        case StepResult::Cancelled: return "cancelled";
        case StepResult::Skipped: return "skipped";
        case StepResult::NoReply: return "no_reply";
    }
    return "pending";
}

const char *runOutcomeName(RunOutcome outcome) {
    switch (outcome) {
        case RunOutcome::Running: return "running";
        case RunOutcome::Completed: return "completed";
        case RunOutcome::CompletedWithErrors: return "completed_with_errors";
        case RunOutcome::Cancelled: return "cancelled";
        case RunOutcome::Rejected: return "rejected";
    }
    return "running";
}

uint8_t ActivityRun::failedSteps() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < stepCount; i++) {
        n += steps[i].result == StepResult::Failed || steps[i].result == StepResult::TimedOut ? 1 : 0;
    }
    return n;
}

void RunHistory::read(ActivityRuns *out) const {
    while (lock_.test_and_set(std::memory_order_acquire)) {
    }
    // Newest first: by id, highest first (an insertion sort of indexes:
    // a handful, and no big copies).
    uint8_t order[kMaxActivityRuns];
    const size_t count = std::min<size_t>(shared_.count, kMaxActivityRuns);
    for (size_t i = 0; i < count; i++) {
        size_t j = i;
        for (; j > 0 && shared_.runs[order[j - 1]].id < shared_.runs[i].id; j--) order[j] = order[j - 1];
        order[j] = static_cast<uint8_t>(i);
    }
    out->count = static_cast<uint8_t>(count);
    for (size_t i = 0; i < count; i++) out->runs[i] = shared_.runs[order[i]];
    lock_.clear(std::memory_order_release);
}

bool RunHistory::readLatest(LastRun *out) const {
    while (lock_.test_and_set(std::memory_order_acquire)) {
    }
    const bool any = shared_.count != 0;
    if (any) {
        const ActivityRun &run = *std::max_element(
            shared_.runs, shared_.runs + shared_.count,
            [](const ActivityRun &a, const ActivityRun &b) { return a.id < b.id; });
        out->id = run.id;
        out->activityId = run.activityId;
        out->kind = run.kind;
        out->trigger = run.trigger;
        out->outcome = run.outcome;
        out->failedSteps = run.failedSteps();
    }
    lock_.clear(std::memory_order_release);
    return any;
}

ActivityRun *RunHistory::start(const SequenceInfo &info, uint8_t stepCount, uint32_t nowMs) {
    // A macro replaces a macro once macros hold their share, so they never
    // crowd out startups and shutdowns. Never a running run: its end would
    // be lost.
    const auto macros = static_cast<size_t>(std::count_if(
        ring_.runs, ring_.runs + ring_.count, [](const ActivityRun &r) { return r.kind == SequenceOrigin::Macro; }));
    const bool macroOnly = info.origin == SequenceOrigin::Macro && macros >= kMaxMacroRuns;
    ActivityRun *slot = nullptr;
    if (!macroOnly && ring_.count < kMaxActivityRuns) {
        slot = &ring_.runs[ring_.count++];
    } else {
        for (size_t i = 0; i < ring_.count; i++) {
            ActivityRun &r = ring_.runs[i];
            if (r.outcome == RunOutcome::Running || (macroOnly && r.kind != SequenceOrigin::Macro)) continue;
            if (slot == nullptr || r.id < slot->id) slot = &r;
        }
        if (slot == nullptr) return nullptr;
    }
    ActivityRun &run = *slot;
    run.id = nextId_++;
    run.activityId = info.activityId;
    run.kind = info.origin;
    run.trigger = info.origin == SequenceOrigin::Macro ? info.trigger : Trigger{};
    run.sequenceId = info.id;
    run.startedMs = nowMs;
    run.durationMs = 0;
    run.outcome = RunOutcome::Running;
    run.stepCount = stepCount;
    for (uint8_t i = 0; i < stepCount; i++) run.steps[i] = RunStep{};
    return &run;
}

uint32_t RunHistory::started(ActivityRun &run, ErrorCode queued, uint32_t nowMs) {
    if (queued != ErrorCode::Ok) finishRun(run, SequenceOutcome::Rejected, queued, nowMs);
    changed(run);
    return run.id;
}

uint32_t RunHistory::begin(const SequenceInfo &info, const CompiledConfig &config, ActionSpan span, ErrorCode queued,
                           uint32_t nowMs) {
    ActivityRun *run = start(info, config.fits(span) ? span.count : 0, nowMs);
    if (run == nullptr) return 0;
    const CompiledAction *actions = config.span(span);
    for (uint8_t i = 0; i < run->stepCount; i++) {
        RunStep &step = run->steps[i];
        if (const CompiledDevice *device = config.deviceOf(actions[i])) step.deviceId = device->id;
        if (actions[i].command != nullptr) step.commandId = actions[i].command->id;
    }
    return started(*run, queued, nowMs);
}

uint32_t RunHistory::begin(const SequenceInfo &info, const CompiledCommand *steps, size_t count, ErrorCode queued,
                           uint32_t nowMs) {
    ActivityRun *run = start(info, static_cast<uint8_t>(std::min<size_t>(count, limits::kMaxSequenceLength)), nowMs);
    if (run == nullptr) return 0;
    for (uint8_t i = 0; i < run->stepCount; i++) {
        if (targetsDevice(steps[i].action)) run->steps[i].deviceId = steps[i].deviceId;
        if (steps[i].action.command != nullptr) run->steps[i].commandId = steps[i].action.command->id;
    }
    return started(*run, queued, nowMs);
}

bool RunHistory::records(SequenceOrigin origin) {
    return origin == SequenceOrigin::Startup || origin == SequenceOrigin::Shutdown || origin == SequenceOrigin::Macro;
}

ActivityRun *RunHistory::current(const SequenceInfo &info) {
    if (!records(info.origin)) return nullptr;
    for (size_t i = 0; i < ring_.count; i++) {
        ActivityRun &run = ring_.runs[i];
        if (run.sequenceId == info.id && run.outcome == RunOutcome::Running) return &run;
    }
    return nullptr;
}

namespace {

void setResult(RunStep &step, const ExecResult &result, uint32_t atMs) {
    switch (result.status) {
        case ExecStatus::Pending: return;
        case ExecStatus::Accepted: step.result = StepResult::Ok; break;
        case ExecStatus::Failed: step.result = StepResult::Failed; break;
        case ExecStatus::Timeout: step.result = StepResult::TimedOut; break;
        case ExecStatus::Skipped: step.result = StepResult::Skipped; break;
        case ExecStatus::NoReply: step.result = StepResult::NoReply; break;
    }
    step.error = succeeded(result.status) ? result.error : failureCode(result);
    step.durationMs = atMs - step.startedMs;
}

}  // namespace

void RunHistory::onDispatched(const DispatchRecord &record) {
    ActivityRun *run = current(record.sequence);
    if (run == nullptr || record.index >= run->stepCount) return;
    endWaits(*run, record.atMs);  // a step starts: the wait before it is over
    RunStep &step = run->steps[record.index];
    step.ran = true;
    step.startedMs = record.atMs;
    // A wait is pending until the next step starts or the run ends.
    if (record.action.op != ActionOp::Wait) setResult(step, record.result, record.atMs);
    changed(*run);
}

void RunHistory::endWaits(ActivityRun &run, uint32_t atMs) {
    // Steps run in order, and an adapter's pending step completes before the
    // next starts: a pending step that ran is a wait.
    for (uint8_t i = 0; i < run.stepCount; i++) {
        RunStep &step = run.steps[i];
        if (!step.ran || step.result != StepResult::Pending) continue;
        step.result = StepResult::Ok;
        step.durationMs = atMs - step.startedMs;
    }
}

void RunHistory::onCompleted(const CompletionRecord &record) {
    ActivityRun *run = current(record.sequence);
    if (run == nullptr || record.index >= run->stepCount) return;
    setResult(run->steps[record.index], record.result, record.atMs);
    changed(*run);
}

void RunHistory::onFinished(const SequenceReport &report) {
    ActivityRun *run = current(report.info);
    if (run == nullptr) return;
    finishRun(*run, report.outcome, report.error, report.atMs);
    changed(*run);
}

void RunHistory::finishRun(ActivityRun &run, SequenceOutcome outcome, ErrorCode error, uint32_t atMs) {
    switch (outcome) {
        case SequenceOutcome::Completed: run.outcome = RunOutcome::Completed; break;
        case SequenceOutcome::CompletedWithErrors: run.outcome = RunOutcome::CompletedWithErrors; break;
        case SequenceOutcome::Cancelled: run.outcome = RunOutcome::Cancelled; break;
        case SequenceOutcome::Rejected: run.outcome = RunOutcome::Rejected; break;
    }
    run.durationMs = atMs - run.startedMs;
    // A wait the run got past is over; one cut off is cancelled below.
    if (outcome == SequenceOutcome::Completed || outcome == SequenceOutcome::CompletedWithErrors) {
        endWaits(run, atMs);
    }
    // What never got a result (not run, or cut off while pending) ends with
    // the run.
    for (uint8_t i = 0; i < run.stepCount; i++) {
        RunStep &step = run.steps[i];
        if (step.result != StepResult::Pending) continue;
        step.result = StepResult::Cancelled;
        step.error = error;
        if (step.ran) step.durationMs = atMs - step.startedMs;
    }
}

void RunHistory::clear() {
    ring_.count = 0;
    dirty_ = 0;
    while (lock_.test_and_set(std::memory_order_acquire)) {
    }
    shared_.count = 0;
    lock_.clear(std::memory_order_release);
}

void RunHistory::publish() {
    if (dirty_ == 0 || lock_.test_and_set(std::memory_order_acquire)) return;
    for (size_t i = 0; i < kMaxActivityRuns; i++) {
        if ((dirty_ & (1u << i)) != 0) shared_.runs[i] = ring_.runs[i];
    }
    shared_.count = ring_.count;
    lock_.clear(std::memory_order_release);
    dirty_ = 0;
}

}  // namespace hubbo
