#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "hubbo/domain/adapter.h"
#include "hubbo/domain/config.h"
#include "hubbo/domain/error.h"
#include "hubbo/engine/compiled.h"

// Validation and compilation of source config into runtime tables.
//
// This is the security and correctness gate: every action must name a
// registered integration, a registered device owned by that integration, a
// command the integration offers for that device, and parameters that match
// the command's typed schema. Anything else is rejected with a structured
// error and a JSON Pointer path into the source document, e.g.
//   {code: "invalid_enum_value", path: "/activities/0/bindings/up:down/0/params/key"}
// Runs on the config writer side; allocates freely; never on the hot path.
namespace hubbo {

struct ValidationIssue {
    ErrorCode code;
    std::string path;  // JSON Pointer (RFC 6901) into the validated document
};

class ValidationReport {
public:
    // Keeps the first kMaxValidationIssues issues; counts the rest.
    void add(ErrorCode code, std::string path);

    bool ok() const { return issues_.empty(); }
    const std::vector<ValidationIssue> &issues() const { return issues_; }
    size_t dropped() const { return dropped_; }

    // True if an issue has this code and path (handy in tests).
    bool has(ErrorCode code, const std::string &path) const;

    // The same issues (and dropped count) with paths under `from` moved
    // under `to`; an empty `from` prefixes every path with `to`
    // (re-rooting issues at a request body).
    ValidationReport rerooted(const std::string &from, const std::string &to) const;

private:
    std::vector<ValidationIssue> issues_;
    size_t dropped_ = 0;
};

// Validates and compiles a whole configuration. Returns nullptr (and fills
// `report`) on any error; a partial config is never produced. Paths are
// rooted at the config document: /devices/N/..., /activities/N/...,
// /global_bindings/<trigger>/N/..., /smart_buttons/slots/<slot>[/<press>/<kind>/N/...]
std::unique_ptr<CompiledConfig> compileConfig(const ConfigSpec &config, const AdapterSet &adapters,
                                              ValidationReport *report);

// Validates one activity as it would be saved into `context` (replacing an
// activity with the same id, or added). Paths are rooted at the activity
// document: /id, /startup/N/..., /bindings/<trigger>/N/params/<name>.
// Use for PUT /activities/{id} and POST /activities/validate (dry run).
ValidationReport validateActivity(const ActivitySpec &activity, const ConfigSpec &context,
                                  const AdapterSet &adapters);

// The one place a device's integration is resolved (design section 3), used
// by compileConfig() and compileDeviceCommand(): the integration must be
// listed in config.integrations, of the device's provider type, enabled, and
// have a registered adapter (whose declared type, if any, matches).
struct IntegrationResolution {
    // Ok, UnknownIntegration (not configured), IntegrationMismatch (type),
    // IntegrationDisabled, AdapterUnavailable (no adapter registered).
    ErrorCode error = ErrorCode::Ok;
    int adapterIndex = -1;  // valid for Ok and IntegrationDisabled
    const IntegrationSpec *integration = nullptr;
};
IntegrationResolution resolveIntegration(const ConfigSpec &config, const AdapterSet &adapters,
                                         const DeviceSpec &device);

// Compiles one command of a registered device for direct execution. Errors:
// NotFound (no such device), IntegrationDisabled, the other resolver errors,
// UnknownCommand (issue at /command_id), ValidationFailed (HoldNotAllowed at
// /command_id: only momentary commands execute directly; params at
// /params/<name>, rooted at the execute request body). `call`: the body's
// call (decodeHaServiceCall), for call_service; whether it's required or
// allowed is the caller's check, as decodeAction's for a config. Its
// service's domain must be the device's HA platform (forbidden_param at
// /call/service), as in a config (.../call/service).
ErrorCode compileDeviceCommand(const ConfigSpec &config, const AdapterSet &adapters, std::string_view deviceId,
                               std::string_view commandId, const std::vector<ParamInput> &params,
                               CompiledCommand *out, ValidationReport *report,
                               const HaServiceCallSpec *call = nullptr);

// Compiles a macro run straight from the editor (POST /macros/run; docs/
// design/macros.md), maybe unsaved, against `config`: as a binding marked a
// macro (1 to kMaxSequenceLength actions, waits ok, holds and their
// releases hold_not_allowed), each step self-contained (its device id and
// provider ref; an activate's activity id as its deviceId). Ok, or
// ValidationFailed with issues rooted at the request body: /actions
// (required: none), /actions/<i>/... HTTP task (it allocates).
ErrorCode compileMacro(const ConfigSpec &config, const AdapterSet &adapters, const std::vector<ActionSpec> &actions,
                       std::vector<CompiledCommand> *out, ValidationReport *report);

// JSON Pointer token escaping ("~" -> "~0", "/" -> "~1").
std::string jsonPointerToken(const std::string &token);

}  // namespace hubbo
