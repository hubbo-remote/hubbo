#include "hubbo/config/json.h"

#include "json_fields.h"
#include "profile_fields.h"

#include <algorithm>
#include <cstring>

#include "hubbo/domain/limits.h"
#include "hubbo/api/remote_port.h"
#include "hubbo/ha/capabilities.h"
#include "hubbo/ha/snapshot_json.h"
#include "hubbo/remote/button.h"
#include "hubbo/remote/input.h"

namespace hubbo::config {

namespace {

constexpr const char *kFormat = "hubbo-config";
constexpr const char *kBackupFormat = "hubbo-backup";
constexpr const char *kIntegrationTypes[] = {kSystemType, kBleHidType, kHomeAssistantType, kMqttType};
constexpr size_t kMaxBindings = remote::kButtonCount * remote::kEventKindCount;
constexpr size_t kMaxTimestampLength = 40;

using namespace detail;

bool decodeAction(JsonVariantConst json, ActionSpec *out, ValidationReport *report, const std::string &path) {
    Fields f(json, report, path);
    f.string("integration_id", &out->integrationId, true, limits::kMaxIdLength);
    f.nullableString("device_id", &out->deviceId, true, limits::kMaxIdLength);
    const bool command = f.string("command_id", &out->commandId, true, limits::kMaxCommandIdLength);
    f.nested("params", true,
             [&](JsonVariantConst v, const std::string &p) { return decodeParams(v, &out->params, report, p); });
    // `call` belongs to call_service alone, which needs it.
    const bool callService = out->commandId == ha::kCallServiceCommandId;
    out->call.reset();
    if (callService || !command) {
        f.nested("call", callService, [&](JsonVariantConst v, const std::string &p) {
            return decodeHaServiceCall(v, &out->call.emplace(), report, p);
        });
    }
    return f.finish();  // a call on another command is an unknown field
}

bool decodeSequence(JsonVariantConst json, std::vector<ActionSpec> *out, ValidationReport *report,
                    const std::string &path) {
    return decodeList(json, limits::kMaxSequenceLength, out, report, path,
                      [&](JsonVariantConst j, ActionSpec *a, const std::string &p) {
                          return decodeAction(j, a, report, p);
                      });
}

bool decodeBindings(JsonVariantConst json, std::vector<BindingSpec> *out, ValidationReport *report,
                    const std::string &path) {
    const auto object = objectWithin(json, kMaxBindings, report, path);
    if (!object) return false;
    bool ok = true;
    out->clear();
    for (JsonPairConst kv : *object) {
        BindingSpec binding;
        binding.trigger = kv.key().c_str();
        ok = decodeSequence(kv.value(), &binding.actions, report, child(path, binding.trigger)) && ok;
        out->push_back(std::move(binding));
    }
    return ok;
}

void encodeAction(const ActionSpec &action, JsonObject out) {
    out["integration_id"] = action.integrationId;
    if (action.deviceId) {
        out["device_id"] = *action.deviceId;
    } else {
        out["device_id"] = nullptr;
    }
    out["command_id"] = action.commandId;
    encodeParams(action.params, out["params"].to<JsonObject>());
    if (action.call) {
        JsonObject call = out["call"].to<JsonObject>();
        call["service"] = action.call->service;
        call["data"] = serialized(action.call->data);  // compact JSON, as decoded
    }
}

void encodeSequence(const std::vector<ActionSpec> &actions, JsonArray out) {
    for (const auto &a : actions) encodeAction(a, out.add<JsonObject>());
}

void encodeBindings(const std::vector<BindingSpec> &bindings, JsonObject out) {
    for (const auto &b : bindings) encodeSequence(b.actions, out[b.trigger].to<JsonArray>());
}

bool decodeProvider(JsonVariantConst json, ProviderSpec *out, ValidationReport *report, const std::string &path) {
    Fields f(json, report, path);
    if (f.string("type", &out->type, true, limits::kMaxTypeLength)) {
        if (out->type == kHomeAssistantType) {
            f.string("entity_id", &out->entityId, true);
            f.string("unique_id", &out->uniqueId, false, limits::kMaxUniqueIdLength);
        } else if (out->type != kBleHidType) {  // ble_hid: only its type
            f.failAt(ErrorCode::WrongType, "type");
        }
    }
    return f.finish();
}

void encodeProvider(const ProviderSpec &provider, JsonObject out) {
    out["type"] = provider.type;
    if (provider.type == kHomeAssistantType) {
        out["entity_id"] = provider.entityId;
        if (!provider.uniqueId.empty()) out["unique_id"] = provider.uniqueId;
    }
}

void decodeKind(Fields &f, std::string *kind, bool required) {
    // The shape only: ConfigService checks it's one of kinds.json. A
    // malformed kind can't be one, so it's unknown_kind too.
    if (f.string("kind", kind, required) && !isValidId(*kind)) {  // isValidId bounds the length
        f.failAt(ErrorCode::UnknownKind, "kind");
    }
}

// Stored: IntegrationConfig. Backup: BackupIntegration (no revision; settings
// optional and without their own `type`).
enum class IntegrationShape : uint8_t { Stored, Backup };

bool decodeIntegrationAs(JsonVariantConst json, IntegrationShape shape, IntegrationSpec *out,
                         ValidationReport *report, const std::string &path) {
    const bool stored = shape == IntegrationShape::Stored;
    Fields f(json, report, path);
    f.string("id", &out->id, true, limits::kMaxIdLength);
    if (f.string("type", &out->type, true, limits::kMaxTypeLength) && !oneOf(out->type, kIntegrationTypes)) {
        f.failAt(ErrorCode::WrongType, "type");
    }
    f.string("name", &out->name, true, limits::kMaxNameLength);
    f.boolean("enabled", &out->enabled, true);
    if (stored) {
        f.revision("revision", &out->revision, true);
    } else {
        out->revision = 1;
    }
    f.nested("settings", stored, [&](JsonVariantConst v, const std::string &p) {
        Fields s(v, report, p);
        if (stored) {
            std::string type;
            if (s.string("type", &type, true, limits::kMaxTypeLength) && type != out->type) {
                s.failAt(ErrorCode::IntegrationMismatch, "type");
            }
        }
        const bool ha = out->type == kHomeAssistantType;
        // Backups name only the fields their type has, all optional.
        if (ha || !stored) {
            s.string("url", &out->url, stored, limits::kMaxUrlLength);
            s.boolean("verify_tls", &out->verifyTls, stored);
        }
        if (out->type == kBleHidType || !stored) {
            s.string("device_name", &out->deviceName, stored, limits::kMaxBleNameLength);
        }
        if (out->type == kMqttType || !stored) {
            s.text("host", &out->host, stored, limits::kMaxHostLength, isValidMqttHost);
            int64_t port = out->port;
            if (s.integer("port", 1, 65535, &port, stored)) out->port = static_cast<uint16_t>(port);
            s.text("username", &out->username, stored, limits::kMaxUsernameLength, isValidMqttUsername);
        }
        return s.finish();
    });
    return f.finish();
}

void encodeIntegrationAs(const IntegrationSpec &integration, IntegrationShape shape, JsonObject out) {
    const bool stored = shape == IntegrationShape::Stored;
    out["id"] = integration.id;
    out["type"] = integration.type;
    out["name"] = integration.name;
    out["enabled"] = integration.enabled;
    JsonObject settings = out["settings"].to<JsonObject>();
    if (stored) settings["type"] = integration.type;
    if (integration.type == kHomeAssistantType) {
        settings["url"] = integration.url;
        settings["verify_tls"] = integration.verifyTls;
    } else if (integration.type == kBleHidType) {
        settings["device_name"] = integration.deviceName;
    } else if (integration.type == kMqttType) {
        settings["host"] = integration.host;
        settings["port"] = integration.port;
        settings["username"] = integration.username;
    }
    if (stored) {
        out["revision"] = integration.revision;
    } else if (settings.size() == 0) {
        out.remove("settings");
    }
}

bool decodeGlobalBindingsAs(JsonVariantConst json, bool backup, std::vector<BindingSpec> *bindings,
                            uint32_t *revision, ValidationReport *report, const std::string &path);

// The sections config.json and backups share: integrations, devices,
// global_bindings, activities.
void decodeSections(Fields &f, IntegrationShape integrations, ConfigSpec *config, ValidationReport *r) {
    const bool backup = integrations == IntegrationShape::Backup;
    f.nested("integrations", true, [&](JsonVariantConst v, const std::string &p) {
        if (!decodeList(v, limits::kMaxIntegrations, &config->integrations, r, p,
                        [&](JsonVariantConst j, IntegrationSpec *o, const std::string &at) {
                            return decodeIntegrationAs(j, integrations, o, r, at);
                        })) {
            return false;
        }
        if (integrationCountWithBuiltins(config->integrations) > limits::kMaxIntegrations) {
            addIssue(r, ErrorCode::TooLong, p);  // the built-ins added at load would pass the cap
            return false;
        }
        return true;
    });
    f.nested("devices", true, [&](JsonVariantConst v, const std::string &p) {
        return decodeList(v, limits::kMaxDevices, &config->devices, r, p,
                          [&](JsonVariantConst j, DeviceSpec *o, const std::string &at) {
                              return decodeDevice(j, backup ? DeviceShape::Backup : DeviceShape::Stored, o, r, at);
                          });
    });
    f.nested("global_bindings", true, [&](JsonVariantConst v, const std::string &p) {
        return decodeGlobalBindingsAs(v, backup, &config->globalBindings, &config->globalBindingsRevision, r, p);
    });
    f.nested("activities", true, [&](JsonVariantConst v, const std::string &p) {
        return decodeList(v, limits::kMaxActivities, &config->activities, r, p,
                          [&](JsonVariantConst j, ActivitySpec *o, const std::string &at) {
                              return decodeActivity(j, backup ? ActivityShape::Backup : ActivityShape::Stored, o, r,
                                                    at);
                          });
    });
}

void encodeSections(const ConfigSpec &config, IntegrationShape integrations, JsonDocument &doc) {
    JsonArray list = doc["integrations"].to<JsonArray>();
    for (const auto &i : config.integrations) encodeIntegrationAs(i, integrations, list.add<JsonObject>());
    JsonArray devices = doc["devices"].to<JsonArray>();
    for (const auto &d : config.devices) encodeDevice(d, devices.add<JsonObject>());
    encodeGlobalBindings(config.globalBindings, config.globalBindingsRevision,
                         doc["global_bindings"].to<JsonObject>());
    JsonArray activities = doc["activities"].to<JsonArray>();
    for (const auto &a : config.activities) encodeActivity(a, activities.add<JsonObject>());
}

std::string toText(const JsonDocument &doc, bool trailingNewline) {
    std::string out;
    out.reserve(measureJson(doc) + 1);
    serializeJson(doc, out);
    if (trailingNewline) out += '\n';
    return out;
}

}  // namespace

ErrorCode parseJson(std::string_view text, JsonDocument &doc, size_t maxBytes, uint8_t nestingLimit) {
    if (text.empty() || text.size() > maxBytes) return ErrorCode::MalformedJson;
    const DeserializationError err =
        deserializeJson(doc, text.data(), text.size(), DeserializationOption::NestingLimit(nestingLimit));
    return err ? ErrorCode::MalformedJson : ErrorCode::Ok;
}

namespace {

// Levels of `v` (a scalar is 0, {} or [] is 1), stopping past `limit`.
size_t depthOf(JsonVariantConst v, size_t limit) {
    if (limit == 0) return v.is<JsonObjectConst>() || v.is<JsonArrayConst>() ? 1 : 0;
    size_t deepest = 0;
    if (v.is<JsonObjectConst>()) {
        for (JsonPairConst kv : v.as<JsonObjectConst>()) deepest = std::max(deepest, depthOf(kv.value(), limit - 1));
        return deepest + 1;
    }
    if (v.is<JsonArrayConst>()) {
        for (JsonVariantConst item : v.as<JsonArrayConst>()) deepest = std::max(deepest, depthOf(item, limit - 1));
        return deepest + 1;
    }
    return 0;
}

bool templated(const char *text) {
    return text != nullptr && (std::strstr(text, "{{") != nullptr || std::strstr(text, "{%") != nullptr);
}

// The path of the first key or string with a template in it, or "".
std::string firstTemplate(JsonVariantConst v, const std::string &path) {
    if (v.is<JsonObjectConst>()) {
        for (JsonPairConst kv : v.as<JsonObjectConst>()) {
            const std::string at = child(path, kv.key().c_str());
            if (templated(kv.key().c_str())) return at;
            std::string found = firstTemplate(kv.value(), at);
            if (!found.empty()) return found;
        }
    } else if (v.is<JsonArrayConst>()) {
        size_t i = 0;
        for (JsonVariantConst item : v.as<JsonArrayConst>()) {
            std::string found = firstTemplate(item, child(path, i++));
            if (!found.empty()) return found;
        }
    } else if (v.is<const char *>() && templated(v.as<const char *>())) {
        return path;
    }
    return {};
}

}  // namespace

bool decodeHaServiceCall(JsonVariantConst json, HaServiceCallSpec *out, ValidationReport *report,
                         const std::string &path) {
    Fields f(json, report, path);
    if (f.string("service", &out->service, true, limits::kMaxHaServiceLength) && !ha::isHaServiceId(out->service)) {
        f.failAt(ErrorCode::Pattern, "service");
    }
    out->data = "{}";
    f.nested("data", false, [&](JsonVariantConst data, const std::string &p) {
        const auto fail = [&](ErrorCode code) {
            addIssue(report, code, p);
            return false;
        };
        if (!data.is<JsonObjectConst>()) return fail(ErrorCode::WrongType);
        if (depthOf(data, limits::kMaxHaCallDataDepth) > limits::kMaxHaCallDataDepth) return fail(ErrorCode::TooDeep);
        std::string text;
        serializeJson(data, text);
        if (text.size() > limits::kMaxHaCallDataBytes) return fail(ErrorCode::TooLong);
        // The target is the device's entity, never one from the data.
        bool ok = true;
        for (JsonPairConst member : data.as<JsonObjectConst>()) {
            if (ha::isTargetKey(member.key().c_str())) {
                addIssue(report, ErrorCode::ForbiddenParam, child(p, member.key().c_str()));
                ok = false;
            }
        }
        // HA would render a template; sent verbatim, it must not be one.
        const std::string at = firstTemplate(data, p);
        if (!at.empty()) {
            addIssue(report, ErrorCode::TemplateNotSupported, at);
            ok = false;
        }
        if (ok) out->data = std::move(text);
        return ok;
    });
    return f.finish();
}

bool decodeParams(JsonVariantConst json, std::vector<ParamInput> *out, ValidationReport *report,
                  const std::string &path) {
    const auto object = objectWithin(json, limits::kMaxParams, report, path);
    if (!object) return false;
    bool ok = true;
    out->clear();
    for (JsonPairConst kv : *object) {
        ParamInput p;
        p.name = kv.key().c_str();
        JsonVariantConst v = kv.value();
        ErrorCode problem = ErrorCode::Ok;
        if (p.name.size() > limits::kMaxIdLength) {
            problem = ErrorCode::TooLong;
        } else if (v.is<bool>()) {
            p.value = ParamLiteral::ofBool(v.as<bool>());
        } else if (v.is<int64_t>()) {
            p.value = ParamLiteral::ofInt(v.as<int64_t>());
        } else if (v.is<double>()) {
            p.value = ParamLiteral::ofNumber(v.as<double>());
        } else if (v.is<const char *>()) {
            const char *s = v.as<const char *>();
            if (std::strlen(s) > limits::kMaxParamStringLength) {
                problem = ErrorCode::TooLong;
            } else {
                p.value = ParamLiteral::ofString(s);
            }
        } else {
            problem = ErrorCode::WrongType;  // null, arrays and objects are never params
        }
        if (problem != ErrorCode::Ok) {
            addIssue(report, problem, child(path, p.name));
            ok = false;
            continue;
        }
        out->push_back(std::move(p));
    }
    return ok;
}

void encodeParams(const std::vector<ParamInput> &params, JsonObject out) {
    for (const auto &p : params) {
        auto v = out[p.name];  // a member proxy: assigning creates the member
        switch (p.value.kind) {
            case ParamLiteral::Kind::Null: v = nullptr; break;
            case ParamLiteral::Kind::Bool: v = p.value.boolean; break;
            case ParamLiteral::Kind::Int: v = p.value.integer; break;
            case ParamLiteral::Kind::Number: v = p.value.number; break;
            case ParamLiteral::Kind::String: v = p.value.string; break;
        }
    }
}

bool decodeActivity(JsonVariantConst json, ActivityShape shape, ActivitySpec *out, ValidationReport *report,
                    const std::string &path) {
    Fields f(json, report, path);
    if (shape != ActivityShape::Create) {
        const bool backup = shape == ActivityShape::Backup;
        f.string("id", &out->id, shape == ActivityShape::Stored || backup, limits::kMaxIdLength);
        f.schemaVersion(shape == ActivityShape::Stored);
        f.revision("revision", &out->revision, shape == ActivityShape::Stored || shape == ActivityShape::Replace,
                   backup);
    }
    f.string("name", &out->name, true, limits::kMaxNameLength);
    f.nested("startup", true,
             [&](JsonVariantConst v, const std::string &p) { return decodeSequence(v, &out->startup, report, p); });
    f.nested("shutdown", true,
             [&](JsonVariantConst v, const std::string &p) { return decodeSequence(v, &out->shutdown, report, p); });
    f.nested("bindings", true,
             [&](JsonVariantConst v, const std::string &p) { return decodeBindings(v, &out->bindings, report, p); });
    return f.finish();
}

void encodeActivity(const ActivitySpec &activity, JsonObject out) {
    out["id"] = activity.id;
    out["name"] = activity.name;
    encodeSequence(activity.startup, out["startup"].to<JsonArray>());
    encodeSequence(activity.shutdown, out["shutdown"].to<JsonArray>());
    encodeBindings(activity.bindings, out["bindings"].to<JsonObject>());
    out["schema_version"] = kSchemaVersion;
    out["revision"] = activity.revision;
}

namespace {

bool decodeGlobalBindingsAs(JsonVariantConst json, bool backup, std::vector<BindingSpec> *bindings,
                            uint32_t *revision, ValidationReport *report, const std::string &path) {
    Fields f(json, report, path);
    f.nested("bindings", true,
             [&](JsonVariantConst v, const std::string &p) { return decodeBindings(v, bindings, report, p); });
    f.revision("revision", revision, !backup, backup);
    return f.finish();
}

}  // namespace

bool decodeGlobalBindings(JsonVariantConst json, std::vector<BindingSpec> *bindings, uint32_t *revision,
                          ValidationReport *report, const std::string &path) {
    return decodeGlobalBindingsAs(json, false, bindings, revision, report, path);
}

void encodeGlobalBindings(const std::vector<BindingSpec> &bindings, uint32_t revision, JsonObject out) {
    encodeBindings(bindings, out["bindings"].to<JsonObject>());
    out["revision"] = revision;
}

bool decodeDevice(JsonVariantConst json, DeviceShape shape, DeviceSpec *out, ValidationReport *report,
                  const std::string &path) {
    Fields f(json, report, path);
    const bool backup = shape == DeviceShape::Backup;
    const bool stored = shape == DeviceShape::Stored || backup;
    if (stored) {
        f.string("id", &out->id, true, limits::kMaxIdLength);
        f.schemaVersion(true);
        f.revision("revision", &out->revision, !backup, backup);
    }
    f.string("name", &out->name, true, limits::kMaxNameLength);
    f.string("integration_id", &out->integrationId, true, limits::kMaxIdLength);
    decodeKind(f, &out->kind, stored);
    f.nested("provider", true, [&](JsonVariantConst v, const std::string &p) {
        return decodeProvider(v, &out->provider, report, p);
    });
    // A Bluetooth device is the profile it was added from; nothing else has
    // one (its fields are then unknown).
    if (out->provider.type == kBleHidType) {
        out->profile.emplace();
        f.nested("profile", true, [&](JsonVariantConst v, const std::string &p) {
            return decodeProfileRef(v, &*out->profile, report, p);
        });
        decodeProfileFields(f, &*out->profile, report);
    }
    // The hub reads capabilities from HA: only the stored shapes carry them
    // (a client sending one gets unknown_field).
    if (stored) {
        f.nested("ha_capabilities", false, [&](JsonVariantConst v, const std::string &p) {
            // In place, not through a snapshot on this frame (stack: the HTTP task).
            if (ha::decodeSnapshot(v, &out->haCapabilities.emplace(), report, p, out->provider.entityId)) return true;
            out->haCapabilities.reset();
            return false;
        });
    }
    return f.finish();
}

void encodeDevice(const DeviceSpec &device, JsonObject out, DeviceDetail detail) {
    out["id"] = device.id;
    out["name"] = device.name;
    out["integration_id"] = device.integrationId;
    out["kind"] = device.kind;
    encodeProvider(device.provider, out["provider"].to<JsonObject>());
    if (device.profile) {
        encodeProfileRef(*device.profile, out["profile"].to<JsonObject>());
        encodeProfileFields(*device.profile, out, detail != DeviceDetail::Summary);
    }
    if (detail == DeviceDetail::Stored && device.haCapabilities) {
        ha::encodeSnapshot(*device.haCapabilities, out["ha_capabilities"].to<JsonObject>());
    }
    out["schema_version"] = kSchemaVersion;
    out["revision"] = device.revision;
}

bool decodeDeviceUpdate(JsonVariantConst json, DeviceUpdate *out, ValidationReport *report, const std::string &path) {
    Fields f(json, report, path);
    std::string s;
    if (f.string("name", &s, false, limits::kMaxNameLength)) out->name = s;
    s.clear();
    decodeKind(f, &s, false);
    if (!s.empty()) out->kind = s;
    f.nested("provider", false, [&](JsonVariantConst v, const std::string &p) {
        ProviderSpec provider;
        if (!decodeProvider(v, &provider, report, p)) return false;
        out->provider = provider;
        return true;
    });
    f.revision("revision", &out->revision, true);
    return f.finish();
}

bool decodeIntegration(JsonVariantConst json, IntegrationSpec *out, ValidationReport *report, const std::string &path) {
    return decodeIntegrationAs(json, IntegrationShape::Stored, out, report, path);
}

void encodeIntegration(const IntegrationSpec &integration, JsonObject out) {
    encodeIntegrationAs(integration, IntegrationShape::Stored, out);
}

ErrorCode migrateConfig(JsonDocument &, int) {
    return ErrorCode::MigrationFailed;  // nothing released yet: no older version to upgrade from
}

std::string serializeConfig(const ConfigSpec &config) {
    JsonDocument doc;
    doc["format"] = kFormat;
    doc["schema_version"] = kConfigFileVersion;
    doc["revision"] = config.revision;
    encodeSections(config, IntegrationShape::Stored, doc);
    return toText(doc, /*trailingNewline=*/true);
}

ParseResult parseConfig(std::string_view text, ConfigSpec *out, Migration migrate) {
    ParseResult result;
    JsonDocument doc;
    if (parseJson(text, doc, kMaxConfigFileBytes, kFileNestingLimit) != ErrorCode::Ok ||
        !doc.is<JsonObjectConst>()) {
        result.code = ErrorCode::MalformedJson;
        return result;
    }
    JsonVariantConst version = doc["schema_version"];
    if (!version.is<int64_t>()) {
        result.code = ErrorCode::ValidationFailed;
        result.report.add(version.isUnbound() ? ErrorCode::Required : ErrorCode::WrongType, "/schema_version");
        return result;
    }
    result.schemaVersion = static_cast<int>(version.as<int64_t>());
    if (result.schemaVersion > kConfigFileVersion || result.schemaVersion < 0) {
        result.code = ErrorCode::UnsupportedSchema;
        return result;
    }
    if (result.schemaVersion < kConfigFileVersion) {
        const ErrorCode migrated = (migrate != nullptr ? migrate : migrateConfig)(doc, result.schemaVersion);
        if (migrated != ErrorCode::Ok) {
            result.code = ErrorCode::MigrationFailed;
            return result;
        }
        doc["schema_version"] = kConfigFileVersion;
    }

    ConfigSpec config;
    Fields f(doc.as<JsonVariantConst>(), &result.report, "");
    std::string format;
    if (f.string("format", &format, true, limits::kMaxTypeLength) && format != kFormat) {
        f.failAt(ErrorCode::WrongType, "format");
    }
    f.get("schema_version", true);  // checked above
    f.revision("revision", &config.revision, true);
    decodeSections(f, IntegrationShape::Stored, &config, &result.report);
    if (!f.finish() || !result.report.ok()) {
        result.code = ErrorCode::ValidationFailed;
        return result;
    }
    *out = std::move(config);
    return result;
}

std::string serializeBackup(const ConfigSpec &config, const BackupInfo &info) {
    JsonDocument doc;
    doc["format"] = kBackupFormat;
    doc["format_version"] = 1;
    if (info.createdAt.empty()) {
        doc["created_at"] = nullptr;
    } else {
        doc["created_at"] = info.createdAt;
    }
    doc["firmware_version"] = info.firmwareVersion;
    doc["board"] = info.board;
    if (info.hasRemote) {
        doc["remote"]["address"] = info.remoteAddress;
        doc["remote"]["channel"] = info.remoteChannel;
    }
    encodeSections(config, IntegrationShape::Backup, doc);
    JsonArray excluded = doc["excluded"].to<JsonArray>();
    for (const char *e : {"admin_password", "api_tokens", "integration_secrets", "ble_bonds"}) excluded.add(e);
    return toText(doc, /*trailingNewline=*/false);
}

ParseResult parseBackup(std::string_view text, ConfigSpec *out, BackupInfo *info, size_t maxBytes) {
    ParseResult result;
    JsonDocument doc;
    if (parseJson(text, doc, maxBytes, kFileNestingLimit) != ErrorCode::Ok || !doc.is<JsonObjectConst>()) {
        result.code = ErrorCode::MalformedJson;
        return result;
    }
    JsonVariantConst version = doc["format_version"];
    if (version.is<int64_t>() && version.as<int64_t>() != 1) {
        result.code = ErrorCode::UnsupportedSchema;
        result.schemaVersion = static_cast<int>(version.as<int64_t>());
        return result;
    }
    result.schemaVersion = 1;

    ConfigSpec config;
    BackupInfo meta;
    ValidationReport *r = &result.report;
    Fields f(doc.as<JsonVariantConst>(), r, "");
    std::string format;
    if (f.string("format", &format, true, limits::kMaxTypeLength) && format != kBackupFormat) {
        f.failAt(ErrorCode::WrongType, "format");
    }
    if (!f.get("format_version", true).isUnbound() && !version.is<int64_t>()) f.failAt(ErrorCode::WrongType, "format_version");
    std::optional<std::string> created;
    if (f.nullableString("created_at", &created, true, kMaxTimestampLength) && created) meta.createdAt = *created;
    f.string("firmware_version", &meta.firmwareVersion, true, limits::kMaxTypeLength);
    f.string("board", &meta.board, true, limits::kMaxTypeLength);
    f.nested("remote", false, [&](JsonVariantConst v, const std::string &p) {
        Fields rf(v, r, p);
        meta.hasRemote = rf.string("address", &meta.remoteAddress, true, 10);
        JsonVariantConst channel = rf.get("channel", true);
        bool channelOk = false;
        if (!channel.isUnbound()) {
            if (!channel.is<int64_t>() || channel.as<int64_t>() < 0 ||
                channel.as<int64_t>() > api::kMaxRemoteChannel) {
                rf.failAt(ErrorCode::WrongType, "channel");
            } else {
                meta.remoteChannel = static_cast<int>(channel.as<int64_t>());
                channelOk = true;
            }
        }
        // One rule for an endpoint: the console's `radio set` (remote_port.h).
        if (meta.hasRemote && channelOk) {
            api::RemoteEndpoint endpoint;
            endpoint.channel = static_cast<uint8_t>(meta.remoteChannel);
            // Backups write all 10 digits (formatRemoteAddress).
            if (meta.remoteAddress.size() != 10 || !api::parseRemoteAddress(meta.remoteAddress, &endpoint.address) ||
                !api::isValidRemoteEndpoint(endpoint)) {
                rf.failAt(ErrorCode::WrongType, "address");
            }
        }
        return rf.finish();
    });
    decodeSections(f, IntegrationShape::Backup, &config, r);
    JsonVariantConst excluded = f.get("excluded", true);
    if (!excluded.isUnbound() && !excluded.is<JsonArrayConst>()) f.failAt(ErrorCode::WrongType, "excluded");

    if (!f.finish() || !r->ok()) {
        result.code = ErrorCode::ValidationFailed;
        return result;
    }
    *out = std::move(config);
    if (info != nullptr) *info = std::move(meta);
    return result;
}

}  // namespace hubbo::config
