#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hubbo/domain/error.h"
#include "hubbo/domain/ids.h"
#include "hubbo/domain/limits.h"
#include "hubbo/engine/compiled.h"
#include "hubbo/engine/executor.h"

// The last few activity runs (startup and shutdown), step by step, for the
// API (docs/design/activity-runs.md). RAM only.
//
// The engine task records into its own ring (no lock) and publish()es the
// runs that changed to a second copy with a try-lock: if a reader holds the
// lock, it publishes on a later poll, so the engine never waits. read() (any task) holds the lock
// only while it copies the published runs out. No allocation: the owner
// creates it once (on the board, in PSRAM) and hands it to
// Engine::setRunHistory.
namespace hubbo {

constexpr size_t kMaxActivityRuns = 8;  // RunHistory's dirty bits are a uint8_t

// Skipped (already set, error AlreadySet) and NoReply count as ok.
enum class StepResult : uint8_t { Pending, Ok, Failed, TimedOut, Cancelled, Skipped, NoReply };
// "pending", "ok", "failed", "timed_out", "cancelled", "skipped", "no_reply"
const char *stepResultName(StepResult result);

enum class RunOutcome : uint8_t { Running, Completed, CompletedWithErrors, Cancelled, Rejected };
// "running", "completed", "completed_with_errors", "cancelled", "rejected"
const char *runOutcomeName(RunOutcome outcome);

struct RunStep {
    IdString deviceId;           // "" for a system step
    const char *commandId = "";  // the catalog's (adapter-owned, lives as long as the adapters)
    StepResult result = StepResult::Pending;
    ErrorCode error = ErrorCode::Ok;  // why it failed, timed out or was cancelled
    bool ran = false;                 // dispatched
    uint32_t startedMs = 0;           // dispatched at (engine clock)
    uint32_t durationMs = 0;          // dispatch to result (a wait: its length), once it has one

    // Whether durationMs means anything: it ran and has its result (not
    // still in flight, or still waiting).
    bool hasDuration() const { return ran && result != StepResult::Pending; }
};

struct ActivityRun {
    uint32_t id = 0;  // RunHistory's, from 1, never reused: a new run is a new id
    IdString activityId;
    SequenceOrigin kind = SequenceOrigin::Startup;  // Startup or Shutdown
    uint32_t sequenceId = 0;  // the executor's
    uint32_t startedMs = 0;   // engine clock (millis): the reader computes "ago"
    uint32_t durationMs = 0;  // start to the end, once not Running
    RunOutcome outcome = RunOutcome::Running;
    uint8_t stepCount = 0;
    RunStep steps[limits::kMaxSequenceLength];

    uint8_t failedSteps() const;  // Failed and TimedOut
};

// The newest run in brief (/status), without copying the whole history.
struct LastRun {
    uint32_t id = 0;  // ActivityRun::id
    IdString activityId;
    SequenceOrigin kind = SequenceOrigin::Startup;
    RunOutcome outcome = RunOutcome::Running;
    uint8_t failedSteps = 0;  // Failed and TimedOut
};

// Newest first: runs[0] is the latest.
struct ActivityRuns {
    // The engine clock when read, filled by the runtime (api::Runtime::
    // activityRuns) for each run's "ago": startedMs is on the same clock.
    uint32_t nowMs = 0;
    uint8_t count = 0;
    ActivityRun runs[kMaxActivityRuns];
};

class RunHistory {
public:
    RunHistory() = default;
    RunHistory(const RunHistory &) = delete;
    RunHistory &operator=(const RunHistory &) = delete;

    // Any task: the published runs.
    void read(ActivityRuns *out) const;
    // Any task: the newest published run in brief. False: no runs yet.
    bool readLatest(LastRun *out) const;

    // Engine task only. begin() once the sequence is queued (`queued` Ok)
    // or refused, with every step, so a rejected or cancelled run still
    // lists them (none if `span` doesn't fit `config`).
    void begin(const SequenceInfo &info, const CompiledConfig &config, ActionSpan span, ErrorCode queued,
               uint32_t nowMs);
    void onDispatched(const DispatchRecord &record);
    void onCompleted(const CompletionRecord &record);
    void onFinished(const SequenceReport &report);
    // Copies the runs that changed to the published ones, unless a reader
    // holds them; then it tries again next time.
    void publish();
    // Forgets every run, published ones too (a reset, as a reboot would).
    // Run ids keep counting.
    void clear();

private:
    struct Ring {
        ActivityRun runs[kMaxActivityRuns];
        uint8_t next = 0;   // where the next run goes
        uint8_t count = 0;  // runs recorded, up to kMaxActivityRuns
        const ActivityRun &newest(size_t i) const { return runs[(next + kMaxActivityRuns - 1 - i) % kMaxActivityRuns]; }
    };

    // The run a report is about: the newest, if it's that run's sequence.
    ActivityRun *current(const SequenceInfo &info);
    void finishRun(ActivityRun &run, SequenceOutcome outcome, ErrorCode error, uint32_t atMs);
    void endWaits(ActivityRun &run, uint32_t atMs);
    void changed(const ActivityRun &run) { dirty_ |= static_cast<uint8_t>(1u << (&run - ring_.runs)); }

    Ring ring_;
    uint32_t nextId_ = 1;
    uint8_t dirty_ = 0;  // bit i: ring_.runs[i] changed since the last publish
    static_assert(kMaxActivityRuns <= 8, "one dirty bit per run");

    Ring shared_;  // ring_ as last published (same order)
    mutable std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
};

}  // namespace hubbo
