#pragma once

#include <cstddef>
#include <cstdint>

// Every bound in the core in one place. Config that exceeds a bound is
// rejected at validation time with a structured error; runtime structures
// that would exceed a bound reject the work (see each owner for the exact
// overflow behaviour). Nothing grows without limit.
namespace hubbo::limits {

// Identifiers (see ids.h for the allowed characters).
constexpr size_t kMaxIdLength = 32;          // integration, device, activity ids
constexpr size_t kMaxCommandIdLength = 65;   // "<32>.<32>", e.g. "media_player.volume_up"
constexpr size_t kMaxNameLength = 64;        // human display names
constexpr size_t kMaxProviderRefLength = 96; // e.g. HA entity/registry id
constexpr size_t kMaxParamStringLength = 32; // enum option strings

// Configuration.
constexpr size_t kMaxParams = 4;             // per action
constexpr size_t kMaxSequenceLength = 32;    // actions in one startup/shutdown/binding
constexpr size_t kMaxActivities = 16;
constexpr size_t kMaxDevices = 64;
constexpr size_t kMaxAdapters = 8;           // integration instances (excl. system)
constexpr size_t kMaxCompiledActions = 2048; // whole compiled config
constexpr size_t kMaxValidationIssues = 32;  // further issues are dropped

// system.wait
constexpr uint32_t kMaxWaitMs = 60000;

// Executor (see engine/executor.h).
constexpr size_t kExecutorActionPool = 64;   // queued actions, all lanes
constexpr size_t kExecutorSequencesPerLane = 16;
constexpr size_t kExecutorInFlight = 8;      // fire-and-track pending results
constexpr uint32_t kDefaultActionTimeoutMs = 5000;

// SnapshotStore (see engine/snapshot.h).
constexpr size_t kMaxSnapshotReaders = 4;
constexpr size_t kMaxRetiredSnapshots = 4;

}  // namespace hubbo::limits
