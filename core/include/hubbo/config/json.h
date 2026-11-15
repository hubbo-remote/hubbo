#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "hubbo/domain/config.h"
#include "hubbo/engine/compiler.h"
#include "hubbo/engine/smart_actions.h"

// JSON <-> config model, shared by persistence (config.json) and the API
// handlers (request bodies). Shapes follow api/openapi.yaml exactly.
//
// Decoders are strict: unknown fields, wrong JSON types, missing required
// fields and over-long strings/arrays are reported as ValidationIssues with a
// JSON Pointer path (`required`, `unknown_field`, `wrong_type`, `too_long`).
// They check *shape* only; semantics (registered devices, commands, params,
// triggers) are the compiler's job. A decoder returns true when it produced
// a value worth compiling (no shape issues).
//
// Parsing is bounded: callers give a byte limit and a nesting limit, and
// anything larger or deeper is `malformed_json`.
namespace hubbo::config {

constexpr int kSchemaVersion = 1;      // of every stored object (Activity, Device: api/openapi.yaml)
// Of the config document (the payload of the "config" record). An older
// version is refused (no migrations before the first release).
constexpr int kConfigFileVersion = 2;
// api/openapi.yaml. 10: the deepest body is a binding's call.data at its
// 4 levels (body, bindings, list, action, call, then data's 4 levels: 9),
// plus one so data a level too deep still parses and gets `too_deep` at
// its path instead of a 400. A restore is a file (kFileNestingLimit).
constexpr uint8_t kRequestNestingLimit = 10;
// 12: the same call.data inside the config file (config, activities, list,
// activity, bindings, list, action, call, data's 4 levels: 11), plus one.
constexpr uint8_t kFileNestingLimit = 12;
constexpr size_t kMaxRequestBytes = 32 * 1024;    // largest API body (activities)
constexpr size_t kMaxConfigFileBytes = 512 * 1024;
constexpr size_t kMaxStringLength = 255;          // any free-form string field

// Parses `text` into `doc`. MalformedJson if not JSON, empty, larger than
// maxBytes or nested deeper than nestingLimit.
ErrorCode parseJson(std::string_view text, JsonDocument &doc, size_t maxBytes, uint8_t nestingLimit);

// ---- activities --------------------------------------------------------
enum class ActivityShape : uint8_t {
    Create,   // ActivityInput: name, startup, shutdown, bindings
    Replace,  // ActivityReplace: + optional id, schema_version; revision required
    Validate, // ActivityReplace for dry runs: id and revision optional
    Stored,   // Activity: + id, schema_version, revision required
    Backup,   // BackupActivity: id required; schema_version, revision optional (revision >= 0)
};

bool decodeActivity(JsonVariantConst json, ActivityShape shape, ActivitySpec *out, ValidationReport *report,
                    const std::string &path = "");
void encodeActivity(const ActivitySpec &activity, JsonObject out);  // Activity (stored/response)

// {bindings, revision}.
bool decodeGlobalBindings(JsonVariantConst json, std::vector<BindingSpec> *bindings, uint32_t *revision,
                          ValidationReport *report, const std::string &path = "");
void encodeGlobalBindings(const std::vector<BindingSpec> &bindings, uint32_t revision, JsonObject out);

// SmartButtons {revision, slots: {light_1, light_2, switch_1, switch_2}},
// each slot a device id or null (docs/design/smart-buttons.md). All four
// slots are required. Decodes the PUT body (revision required). The config
// file and backups carry the same object as `smart_buttons`, a section that
// may be absent (four empty slots); present, its revision is required in
// the config file and optional in a backup.
bool decodeSmartButtons(JsonVariantConst json, SmartButtonsSpec *out, ValidationReport *report,
                        const std::string &path = "");
void encodeSmartButtons(const SmartButtonsSpec &spec, JsonObject out);
// A slot's derived actions (engine/smart_actions.h): {button, plus, minus},
// each {tap, hold, release} lists of actions. Read-only (GET).
void encodeSmartActions(const SmartSlotActions &actions, JsonObject out);

// Action and binding pieces, for the execute endpoint and partial bodies.
bool decodeParams(JsonVariantConst json, std::vector<ParamInput> *out, ValidationReport *report,
                  const std::string &path);
void encodeParams(const std::vector<ParamInput> &params, JsonObject out);
// An action's `call` (HaServiceCall): `service` must match ha::isHaServiceId
// (pattern); `data` (default {}) must be an object (wrong_type) of at most
// limits::kMaxHaCallDataBytes compact (too_long) and kMaxHaCallDataDepth
// levels (too_deep), with no top-level target key (forbidden_param at
// data/<key>) and no "{{" or "{%" in any key or string
// (template_not_supported at the first one). `out->data` is the compact
// text. Paths are under `path` (the call's).
bool decodeHaServiceCall(JsonVariantConst json, HaServiceCallSpec *out, ValidationReport *report,
                         const std::string &path);

// ---- devices -----------------------------------------------------------
enum class DeviceShape : uint8_t {
    Register,  // DeviceRegister: name, integration_id, kind?, provider
    Stored,    // Device: + id, schema_version, revision (availability ignored)
    Backup,    // StoredDevice: like Stored, but revision optional (>= 0), no availability
};

// A ble_hid device carries its profile: `profile` {id, revision, sha256}
// plus `manufacturer`, `model`, `commands` (required) and `buttons`,
// `power`, `timing`, `inputs`, `channels` (optional), in the spec's shape
// (config/profile.h). Any other device has none of them (unknown_field).
// Register takes them too (the API's add-from-profile builds the device
// with deviceFromProfile(), not from a body).
bool decodeDevice(JsonVariantConst json, DeviceShape shape, DeviceSpec *out, ValidationReport *report,
                  const std::string &path = "");

// What encodeDevice writes. Never `availability` or `platform` (the API adds
// them).
enum class DeviceDetail : uint8_t {
    Stored,   // config record, backup: everything, `ha_capabilities` too
    Full,     // GET /devices/{id}: everything but `ha_capabilities`
    Summary,  // GET /devices: + profile identity, manufacturer, model; no
              // commands, buttons, power, timing, inputs, channels
};
void encodeDevice(const DeviceSpec &device, JsonObject out, DeviceDetail detail = DeviceDetail::Stored);

// DeviceUpdate: partial; revision required.
struct DeviceUpdate {
    std::optional<std::string> name;
    std::optional<std::string> kind;
    std::optional<ProviderSpec> provider;
    std::optional<ha::CapabilitySnapshot> haCapabilities;  // a refresh (set by the hub, never decoded)
    uint32_t revision = 0;
};
bool decodeDeviceUpdate(JsonVariantConst json, DeviceUpdate *out, ValidationReport *report,
                        const std::string &path = "");

// ---- integrations (non-secret) -------------------------------------------
// IntegrationConfig as stored. Read-only, runtime and secret fields are
// unknown here: rejected on input and never written. The API layer adds the
// read-only ones to responses.
bool decodeIntegration(JsonVariantConst json, IntegrationSpec *out, ValidationReport *report,
                       const std::string &path = "");
void encodeIntegration(const IntegrationSpec &integration, JsonObject out);

// ---- whole config file ---------------------------------------------------
// {"format":"hubbo-config","schema_version":1,"revision":N,
//  "integrations":[...],"devices":[...],"global_bindings":{...},"smart_buttons":{...},
//  "activities":[...]}
std::string serializeConfig(const ConfigSpec &config);

// Migration hook: upgrade `doc` in place from `fromVersion` to
// kConfigFileVersion. Return Ok, or an error to reject the file.
using Migration = ErrorCode (*)(JsonDocument &doc, int fromVersion);

// The built-in migrations. Nothing is released yet, so there are none: an
// older version is MigrationFailed (the hub boots Blocked).
ErrorCode migrateConfig(JsonDocument &doc, int fromVersion);

struct ParseResult {
    ErrorCode code = ErrorCode::Ok;  // Ok, MalformedJson, UnsupportedSchema, ValidationFailed
    int schemaVersion = 0;           // as found in the file
    ValidationReport report;         // shape issues (ValidationFailed)
};

// Parses a config document. Shape only: compile the result before trusting
// it. Documents with an older schema_version go through `migrate` (the
// built-in migrateConfig if null); newer ones are UnsupportedSchema, and a
// failed migration is MigrationFailed.
ParseResult parseConfig(std::string_view text, ConfigSpec *out, Migration migrate = nullptr);

// ---- backup archive (GET /admin/backup, POST /admin/restore) -----------
// The Backup schema: the user-facing config without secrets. Integrations
// carry no revision there (restored ones start at 1).
struct BackupInfo {
    std::string createdAt;        // RFC 3339, or "" (null)
    std::string firmwareVersion;
    std::string board;
    bool hasRemote = false;       // radio pairing (not a credential)
    std::string remoteAddress;    // ^[0-9A-F]{10}$
    int remoteChannel = 0;
};

std::string serializeBackup(const ConfigSpec &config, const BackupInfo &info);
// Shape only, like parseConfig(); compile before restoring. `info` optional.
ParseResult parseBackup(std::string_view text, ConfigSpec *out, BackupInfo *info = nullptr,
                        size_t maxBytes = 256 * 1024);

}  // namespace hubbo::config
