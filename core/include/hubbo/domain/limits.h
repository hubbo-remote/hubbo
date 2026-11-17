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
constexpr size_t kMaxFirmwareVersionLength = 64;  // a build's version (a dev build's git describe), as the image marker's
constexpr size_t kMaxUrlLength = 200;        // Home Assistant base URL
constexpr size_t kMaxUniqueIdLength = 64;    // HA entity registry id
constexpr size_t kMaxBleNameLength = 29;     // advertised BLE name (fits the scan response)
constexpr size_t kMaxHostLength = 253;       // MQTT broker: an IP or a DNS name
constexpr size_t kMaxUsernameLength = 64;    // MQTT broker username

// Configuration.
constexpr size_t kMaxParams = 4;             // per action
constexpr size_t kMaxSequenceLength = 32;    // actions in one startup/shutdown/binding
constexpr size_t kMaxActivities = 16;
constexpr size_t kMaxDevices = 64;
constexpr size_t kMaxBleDevices = 4;         // Bluetooth hosts (bonds and connections at once)
constexpr size_t kMaxAdapters = 8;           // integration instances (excl. system)
// The integrations list, built-ins included: the adapters (ble and ha_main
// are two of them) plus system and mqtt_main, which have none. Decode and
// compile both check it, counting built-ins not yet added.
constexpr size_t kMaxIntegrations = kMaxAdapters + 2;
constexpr size_t kMaxCompiledActions = 2048; // whole compiled config
constexpr size_t kMaxValidationIssues = 32;  // further issues are dropped
constexpr size_t kMaxEnumOptions = 128;      // options of one enum param
static_assert(kMaxParams <= 8, "the params-present mask is one byte");

// A Home Assistant action (HaServiceCallSpec, config.h): "<domain>.<service>",
// and its data as compact JSON.
constexpr size_t kMaxHaServiceLength = 97;      // 32 + "." + 64
constexpr size_t kMaxHaCallDataBytes = 1024;
constexpr size_t kMaxHaCallDataDepth = 4;       // the data object is level 1
// Actions with a call in one config: the size budget. 16 of the largest
// (about 1.2 KB each in the record) fit one activity request
// (config::kMaxRequestBytes) and leave the record (kMaxConfigFileBytes)
// its room for everything else.
constexpr size_t kMaxHaServiceCalls = 16;

// system.wait
constexpr uint32_t kMaxWaitMs = 60000;

// Device profiles (~/src/hubbo-devices/docs/spec.md), as a device carries them.
constexpr size_t kMaxProfileCommands = 128;     // the spec allows 512; the hub keeps Bluetooth small
constexpr size_t kMaxProfileCommandNameLength = 32;
constexpr size_t kMaxProfileLabelLength = 32;
constexpr size_t kMaxProfileSteps = 16;         // per power/input step list
constexpr size_t kMaxProfileInputs = 64;
constexpr size_t kMaxProfileIdLength = 56;      // "<source>:<slug>", slug 1-48
constexpr uint32_t kMaxHoldMs = 10000;          // a command's or step's hold_ms
constexpr uint32_t kMaxStepDelayMs = 60000;     // delay_ms, on_delay_ms, input_delay_ms
constexpr uint32_t kMaxKeyDelayMs = 5000;       // timing.key_delay_ms

// What Hubbo's HID report map can carry. A usage outside these needs a new
// descriptor (every host pairs again). The firmware static_asserts its
// report map against them.
constexpr uint16_t kHidKeyboardUsageMin = 0x04;
constexpr uint16_t kHidKeyboardUsageMax = 0x65;
constexpr uint16_t kHidConsumerUsageMin = 0x0001;
constexpr uint16_t kHidConsumerUsageMax = 0x03FF;

// Executor (see engine/executor.h).
constexpr size_t kExecutorActionPool = 64;   // queued actions, all lanes
constexpr size_t kExecutorSequencesPerLane = 16;  // Transition and Input lanes
constexpr size_t kExecutorMacroSequences = 4;     // the Macro lane's queued ones, besides the running one
constexpr size_t kExecutorMacroNodes = 32;  // pool nodes all macros together may hold (presses keep the rest)
constexpr size_t kExecutorInFlight = 8;      // fire-and-track pending results
constexpr uint32_t kDefaultActionTimeoutMs = 5000;

// SnapshotStore (see engine/snapshot.h).
constexpr size_t kMaxSnapshotReaders = 4;
constexpr size_t kMaxRetiredSnapshots = 4;

}  // namespace hubbo::limits
