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
constexpr size_t kMaxParamStringLength = 64; // enum option strings
constexpr size_t kMaxTypeLength = 32;        // integration/provider type names
constexpr size_t kMaxUrlLength = 200;        // Home Assistant base URL
constexpr size_t kMaxUniqueIdLength = 64;    // HA entity registry id
constexpr size_t kMaxBleNameLength = 29;     // advertised BLE name (fits the scan response)

// Configuration.
constexpr size_t kMaxParams = 4;             // per action
constexpr size_t kMaxSequenceLength = 32;    // actions in one startup/shutdown/binding
constexpr size_t kMaxActivities = 16;
constexpr size_t kMaxDevices = 64;
constexpr size_t kMaxBleDevices = 4;         // Bluetooth hosts (bonds and connections at once)
constexpr size_t kMaxAdapters = 8;           // integration instances (excl. system)
constexpr size_t kMaxCompiledActions = 2048; // whole compiled config
constexpr size_t kMaxValidationIssues = 32;  // further issues are dropped
constexpr size_t kMaxEnumOptions = 128;      // options of one enum param
static_assert(kMaxParams <= 8, "the params-present mask is one byte");

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
