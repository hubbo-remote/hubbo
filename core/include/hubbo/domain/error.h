#pragma once

#include <cstdint>

// Structured error codes shared by validation, compilation, execution and
// the API. The snake_case name (errorCodeName) is the stable public form used
// in API error bodies; never rename one, only append.
namespace hubbo {

enum class ErrorCode : uint8_t {
    Ok,

    // Identifiers and names
    InvalidId,           // malformed id (charset/length)
    InvalidName,         // empty or too long display name
    InvalidCommandId,    // malformed command id
    DuplicateId,         // two activities/devices with one id

    // Action references
    UnknownIntegration,  // integration_id not registered
    UnknownDevice,       // device_id not in the registry
    DeviceRequired,      // non-system action without device_id
    DeviceNotAllowed,    // system action with a device_id
    IntegrationMismatch, // device belongs to another integration
    UnknownCommand,      // command not offered for this device

    // Parameters
    UnknownParam,
    DuplicateParam,
    MissingParam,
    InvalidParamType,
    ParamOutOfRange,
    InvalidEnumValue,
    TooManyParams,

    // Activities and triggers
    InvalidTrigger,      // not "<button>:<event>" with known names
    DuplicateTrigger,
    UnknownActivity,
    MisplacedTransition, // system.activate/off not last in a binding, or in startup/shutdown
    TooManyActions,      // sequence longer than kMaxSequenceLength
    TooManyActivities,
    TooManyDevices,
    CapacityExceeded,    // compiled config too large

    // Runtime
    NoActiveActivity,
    QueueFull,           // executor overflow; sequence rejected whole
    Cancelled,           // superseded by an activity transition
    AdapterFailed,
    AdapterTimeout,
    AdapterUnavailable,  // e.g. BLE host not connected
    NotImplemented,      // feature-flagged / unimplemented adapter
    Busy,                // config swap: old snapshots still in use
    Internal,

    Count
};

const char *errorCodeName(ErrorCode code);
bool errorCodeFromName(const char *name, ErrorCode *out);

}  // namespace hubbo
