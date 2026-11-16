#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hubbo/domain/error.h"
#include "hubbo/domain/ids.h"
#include "hubbo/domain/limits.h"
#include "hubbo/engine/compiled.h"
#include "hubbo/engine/executor.h"

// The last few runs (startups, shutdowns and macros), step by step, for the
// API (docs/design/activity-runs.md, macros.md). RAM only.
//
// The engine task records into its own ring (no lock) and publish()es the
// runs that changed to a second copy with a try-lock: if a reader holds the
// lock, it publishes on a later poll, so the engine never waits. read() (any task) holds the lock
// only while it copies the published runs out. No allocation: the owner
// creates it once (on the board, in PSRAM) and hands it to
// Engine::setRunHistory.
namespace hubbo {

constexpr size_t kMaxActivityRuns = 8;  // RunHistory's dirty bits are a uint8_t
// Of those, macros hold at most the Macro lane's worth (the running one and
// the queued), so every accepted macro is kept and startups and shutdowns
// keep the rest.
constexpr size_t kMaxMacroRuns = limits::kExecutorMacroSequences + 1;
static_assert(kMaxMacroRuns < kMaxActivityRuns, "room for startups and shutdowns");

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
    IdString activityId;  // "" for a global binding's macro
    SequenceOrigin kind = SequenceOrigin::Startup;  // Startup, Shutdown or Macro
    Trigger trigger;  // Macro only: its button and event (Hold: long press, else press)
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
    Trigger trigger;  // ActivityRun::trigger
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

    // Whether runs of this origin are recorded (Startup, Shutdown, Macro).
    static bool records(SequenceOrigin origin);

    // Engine task only. begin() once the sequence is queued (`queued` Ok)
    // or refused, with every step, so a rejected or cancelled run still
    // lists them (none if `span` doesn't fit `config`).
    // Each returns the new run's id, or 0: not kept (every run it could
    // replace is still running).
    uint32_t begin(const SequenceInfo &info, const CompiledConfig &config, ActionSpan span, ErrorCode queued,
                   uint32_t nowMs);
    // A macro run from the editor: its self-contained steps.
    uint32_t begin(const SequenceInfo &info, const CompiledCommand *steps, size_t count, ErrorCode queued,
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
    // Slots, not in order: runs[0, count) are kept; ids order them.
    struct Ring {
        ActivityRun runs[kMaxActivityRuns];
        uint8_t count = 0;  // runs recorded, up to kMaxActivityRuns
    };

    // The running run a report is about (its sequence id), if still kept.
    ActivityRun *current(const SequenceInfo &info);
    // A new run in a free slot, else in place of the oldest finished one
    // (a macro's, once macros hold kMaxMacroRuns), its steps still to fill.
    // nullptr: every candidate is still running.
    ActivityRun *start(const SequenceInfo &info, uint8_t stepCount, uint32_t nowMs);
    // Its id, after the steps are filled.
    uint32_t started(ActivityRun &run, ErrorCode queued, uint32_t nowMs);
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
