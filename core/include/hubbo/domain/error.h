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

    // Config service, storage and JSON (Phase 2). Names match api/openapi.yaml.
    MalformedJson,      // not JSON, too deep or too large
    Required,           // a required field is missing
    UnknownField,       // a field the schema doesn't have
    WrongType,          // a field of the wrong JSON type
    TooLong,            // a string or array over its bound
    NotFound,           // no such activity/device/integration (or file)
    RevisionConflict,   // stale `revision`; see current revision
    DeviceInUse,        // device referenced by activities/global bindings
    ValidationFailed,   // see the issue list
    LimitReached,       // too many activities/devices to add another
    StorageFailed,      // read/write/rename failed
    UnsupportedSchema,  // schema_version newer than this firmware understands
    Referenced,         // activity still targeted by a system.activate
    IntegrationDisabled,  // the device's integration is disabled
    ConfigBlocked,      // the saved config can't be used; only restore/reset may write
    MigrationFailed,    // a schema migration couldn't upgrade the saved config
    UnpairedHold,       // a hold command (e.g. key_down) without its release in the matching :up binding
    HoldNotAllowed,     // a hold command where nothing would release it (direct execute, startup, ...)
    Corrupt,            // a stored record exists but no copy of it is intact (never "absent")
    EntityNotFound,     // the provider no longer knows the target (HA: entity removed or renamed)
    SequenceOnly,       // a wait command outside a startup or shutdown (a binding, direct execute)
    UnknownKind,        // a device kind that isn't in kinds.json
    // A Home Assistant action's call (HaServiceCallSpec).
    Pattern,               // a string that doesn't match its pattern (call.service)
    ForbiddenParam,        // a target key in call.data (entity_id, device_id, ...)
    TooDeep,               // call.data nested deeper than limits::kMaxHaCallDataDepth
    TemplateNotSupported,  // "{{" or "{%" in call.data
    // A command that needs the entity's current state (cover.position_step's
    // current_position) couldn't get it: unknown, unavailable or unread.
    StateUnavailable,
    // Not an error: why a startup or shutdown step was skipped (its entity
    // was already in the state the step sets; ExecStatus::Skipped).
    AlreadySet,

    Count
};

const char *errorCodeName(ErrorCode code);
bool errorCodeFromName(const char *name, ErrorCode *out);

}  // namespace hubbo
