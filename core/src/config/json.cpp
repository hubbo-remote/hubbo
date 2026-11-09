#include "hubbo/config/json.h"

#include <cstring>
#include <limits>

#include "hubbo/domain/limits.h"
#include "hubbo/api/remote_port.h"
#include "hubbo/ha/snapshot_json.h"
#include "hubbo/remote/button.h"
#include "hubbo/remote/input.h"

namespace hubbo::config {

namespace {

constexpr const char *kFormat = "hubbo-config";
constexpr const char *kBackupFormat = "hubbo-backup";
constexpr const char *kDeviceKinds[] = {"tv", "receiver", "speaker", "streamer", "console", "light", "switch", "other"};
constexpr const char *kIntegrationTypes[] = {kSystemType, kBleHidType, kHomeAssistantType};
constexpr size_t kMaxBindings = remote::kButtonCount * remote::kEventKindCount;
constexpr size_t kMaxKindLength = 16;
constexpr size_t kMaxTimestampLength = 40;

template <size_t N>
bool oneOf(const std::string &value, const char *const (&options)[N]) {
    for (const char *o : options) {
        if (value == o) return true;
    }
    return false;
}

std::string child(const std::string &path, std::string_view key) {
    return path + "/" + jsonPointerToken(std::string(key));
}

std::string child(const std::string &path, size_t index) { return path + "/" + std::to_string(index); }

void addIssue(ValidationReport *report, ErrorCode code, const std::string &path) {
    if (report != nullptr) report->add(code, path);
}

// Strict reader for one JSON object: every key must be asked for, or it is
// reported as unknown in finish(). Error paths are only built on failure.
class Fields {
public:
    Fields(JsonVariantConst json, ValidationReport *report, std::string path)
        : report_(report), path_(std::move(path)) {
        if (!json.is<JsonObjectConst>()) {
            fail(ErrorCode::WrongType, path_);
            return;
        }
        object_ = json.as<JsonObjectConst>();
    }

    bool isObject() const { return !object_.isNull(); }
    std::string at(const char *key) const { return child(path_, key); }

    // The raw value; Required reported if missing and required.
    JsonVariantConst get(const char *key, bool required) {
        if (knownCount_ < kMaxFields) known_[knownCount_++] = key;
        if (object_.isNull()) return {};
        JsonVariantConst v = object_[key];
        if (v.isUnbound() && required) failAt(ErrorCode::Required, key);
        return v;
    }

    bool string(const char *key, std::string *out, bool required, size_t maxLength = kMaxStringLength) {
        JsonVariantConst v = get(key, required);
        return !v.isUnbound() && readString(v, key, out, maxLength);
    }

    // A string that may be JSON null (-> nullopt). True if present and valid.
    bool nullableString(const char *key, std::optional<std::string> *out, bool required, size_t maxLength) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        if (v.isNull()) {
            out->reset();
            return true;
        }
        std::string s;
        if (!readString(v, key, &s, maxLength)) return false;
        *out = std::move(s);
        return true;
    }

    bool boolean(const char *key, bool *out, bool required) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        if (!v.is<bool>()) return failAt(ErrorCode::WrongType, key);
        *out = v.as<bool>();
        return true;
    }

    // Revision: integer 1..UINT32_MAX (0..UINT32_MAX as found in backups,
    // where restore re-stamps it anyway).
    bool revision(const char *key, uint32_t *out, bool required, bool allowZero = false) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        const int64_t lowest = allowZero ? 0 : 1;
        if (!v.is<int64_t>() || v.as<int64_t>() < lowest ||
            v.as<int64_t>() > std::numeric_limits<uint32_t>::max()) {
            return failAt(ErrorCode::WrongType, key);
        }
        *out = static_cast<uint32_t>(v.as<int64_t>());
        return true;
    }

    void schemaVersion(bool required) {
        JsonVariantConst v = get("schema_version", required);
        if (v.isUnbound()) return;
        if (!v.is<int64_t>()) {
            failAt(ErrorCode::WrongType, "schema_version");
        } else if (v.as<int64_t>() != kSchemaVersion) {
            failAt(ErrorCode::UnsupportedSchema, "schema_version");
        }
    }

    // A nested value handed to `decode(json, path) -> bool`; a failure there
    // (which reports its own issues) fails this object too.
    template <typename Decode>
    void nested(const char *key, bool required, Decode decode) {
        JsonVariantConst v = get(key, required);
        if (!v.isUnbound() && !decode(v, at(key))) ok_ = false;
    }

    // Reports unknown keys; returns whether everything was fine.
    bool finish() {
        if (object_.isNull()) return ok_;
        for (JsonPairConst kv : object_) {
            bool known = false;
            for (size_t i = 0; i < knownCount_ && !known; i++) known = std::strcmp(known_[i], kv.key().c_str()) == 0;
            if (!known) fail(ErrorCode::UnknownField, child(path_, kv.key().c_str()));
        }
        return ok_;
    }

    void fail(ErrorCode code, const std::string &path) {
        ok_ = false;
        addIssue(report_, code, path);
    }

    bool failAt(ErrorCode code, const char *key) {
        fail(code, at(key));
        return false;
    }

private:
    static constexpr size_t kMaxFields = 16;  // more than any schema object has

    bool readString(JsonVariantConst v, const char *key, std::string *out, size_t maxLength) {
        if (!v.is<const char *>()) return failAt(ErrorCode::WrongType, key);
        const char *s = v.as<const char *>();
        const size_t n = std::strlen(s);
        if (n > maxLength) return failAt(ErrorCode::TooLong, key);
        out->assign(s, n);
        return true;
    }

    ValidationReport *report_;
    std::string path_;
    JsonObjectConst object_;
    const char *known_[kMaxFields] = {};
    size_t knownCount_ = 0;
    bool ok_ = true;
};

// A JSON array of at most `maxItems`, each decoded by
// `decode(json, T*, path) -> bool`. Replaces *out.
template <typename T, typename Decode>
bool decodeList(JsonVariantConst json, size_t maxItems, std::vector<T> *out, ValidationReport *report,
                const std::string &path, Decode decode) {
    if (!json.is<JsonArrayConst>()) {
        addIssue(report, ErrorCode::WrongType, path);
        return false;
    }
    JsonArrayConst array = json.as<JsonArrayConst>();
    if (array.size() > maxItems) {
        addIssue(report, ErrorCode::TooLong, path);
        return false;
    }
    bool ok = true;
    out->clear();
    size_t i = 0;
    for (JsonVariantConst item : array) {
        T value;
        ok = decode(item, &value, child(path, i++)) && ok;
        out->push_back(std::move(value));
    }
    return ok;
}

// A JSON object of at most `maxEntries`, or nullopt (issue reported).
std::optional<JsonObjectConst> objectWithin(JsonVariantConst json, size_t maxEntries, ValidationReport *report,
                                            const std::string &path) {
    if (!json.is<JsonObjectConst>()) {
        addIssue(report, ErrorCode::WrongType, path);
        return std::nullopt;
    }
    JsonObjectConst object = json.as<JsonObjectConst>();
    if (object.size() > maxEntries) {
        addIssue(report, ErrorCode::TooLong, path);
        return std::nullopt;
    }
    return object;
}

bool decodeAction(JsonVariantConst json, ActionSpec *out, ValidationReport *report, const std::string &path) {
    Fields f(json, report, path);
    f.string("integration_id", &out->integrationId, true, limits::kMaxIdLength);
    f.nullableString("device_id", &out->deviceId, true, limits::kMaxIdLength);
    f.string("command_id", &out->commandId, true, limits::kMaxCommandIdLength);
    f.nested("params", true,
             [&](JsonVariantConst v, const std::string &p) { return decodeParams(v, &out->params, report, p); });
    return f.finish();
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
        } else if (out->type == kBleHidType) {
            f.string("peer_name", &out->peerName, false, limits::kMaxPeerNameLength);
        } else {
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
    } else if (!provider.peerName.empty()) {
        out["peer_name"] = provider.peerName;
    }
}

void decodeKind(Fields &f, std::string *kind, bool required) {
    if (f.string("kind", kind, required, kMaxKindLength) && !oneOf(*kind, kDeviceKinds)) {
        f.failAt(ErrorCode::WrongType, "kind");
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
            // Retired (never had a UI): a record or backup that has it still
            // loads, and the next save leaves it out.
            s.get("ca_cert_pem", false);
        }
        if (out->type == kBleHidType || !stored) {
            s.string("device_name", &out->deviceName, stored, limits::kMaxBleNameLength);
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
        return decodeList(v, limits::kMaxAdapters + 1, &config->integrations, r, p,
                          [&](JsonVariantConst j, IntegrationSpec *o, const std::string &at) {
                              return decodeIntegrationAs(j, integrations, o, r, at);
                          });
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

// BLE HID key params renamed after config version 1: DIGIT_n -> KEY_n.
// Walks every action's params in activities and global bindings.
void renameLegacyKeys(JsonDocument &doc) {
    auto fixActions = [](JsonVariant actions) {
        for (JsonVariant action : actions.as<JsonArray>()) {
            JsonVariant key = action["params"]["key"];
            if (!key.is<const char *>()) continue;
            const std::string value = key.as<const char *>();
            if (value.size() == 7 && value.rfind("DIGIT_", 0) == 0 && value[6] >= '0' && value[6] <= '9') {
                key.set(std::string("KEY_") + value[6]);
            }
        }
    };
    auto fixBindings = [&](JsonVariant bindings) {
        for (JsonPair kv : bindings.as<JsonObject>()) fixActions(kv.value());
    };
    for (JsonVariant activity : doc["activities"].as<JsonArray>()) {
        fixActions(activity["startup"]);
        fixActions(activity["shutdown"]);
        fixBindings(activity["bindings"]);
    }
    fixBindings(doc["global_bindings"]["bindings"]);
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
        if (!backup) f.get("availability", false);  // runtime state; accepted and dropped
    }
    f.string("name", &out->name, true, limits::kMaxNameLength);
    f.string("integration_id", &out->integrationId, true, limits::kMaxIdLength);
    decodeKind(f, &out->kind, stored);
    f.nested("provider", true, [&](JsonVariantConst v, const std::string &p) {
        return decodeProvider(v, &out->provider, report, p);
    });
    // The hub reads capabilities from HA: only the stored shapes carry them
    // (a client sending one gets unknown_field).
    if (stored) {
        f.nested("ha_capabilities", false, [&](JsonVariantConst v, const std::string &p) {
            ha::CapabilitySnapshot snapshot;
            if (!ha::decodeSnapshot(v, &snapshot, report, p)) return false;
            out->haCapabilities = std::move(snapshot);
            return true;
        });
    }
    return f.finish();
}

void encodeDevice(const DeviceSpec &device, JsonObject out, bool withCapabilities) {
    out["id"] = device.id;
    out["name"] = device.name;
    out["integration_id"] = device.integrationId;
    out["kind"] = device.kind;
    encodeProvider(device.provider, out["provider"].to<JsonObject>());
    if (withCapabilities && device.haCapabilities) ha::encodeSnapshot(*device.haCapabilities, out["ha_capabilities"].to<JsonObject>());
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

ErrorCode migrateConfig(JsonDocument &doc, int fromVersion) {
    if (fromVersion < 1) return ErrorCode::MigrationFailed;
    if (fromVersion < 2) renameLegacyKeys(doc);
    return ErrorCode::Ok;
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
    renameLegacyKeys(doc);  // backups made before config version 2

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
