#include "support.h"

#include <cstdio>

#include "hubbo/config/json.h"
#include "hubbo/config/service.h"

namespace hubbo::api {

void sendJson(http::Response &out, int status, const JsonDocument &doc) {
    std::string body;
    body.reserve(measureJson(doc));
    serializeJson(doc, body);
    out.json(status, std::move(body));
}

void sendEmpty(http::Response &out, int status) {
    out.status = status;
    out.contentType.clear();
    out.body.clear();
}

void sendError(http::Response &out, int status, std::string_view code, std::string_view message) {
    http::jsonError(out, status, code, message);
}

JsonObject errorBody(JsonDocument &doc, std::string_view code, std::string_view message) {
    JsonObject error = doc["error"].to<JsonObject>();
    error["code"] = code;
    error["message"] = message;
    return error;
}

const char *issueMessage(ErrorCode code) {
    switch (code) {
        case ErrorCode::Required: return "Required field is missing";
        case ErrorCode::UnknownField: return "Unknown field";
        case ErrorCode::WrongType: return "Wrong JSON type";
        case ErrorCode::TooLong: return "Too long or too many items";
        case ErrorCode::InvalidId: return "Malformed id";
        case ErrorCode::InvalidName: return "Name must be 1-64 characters without control characters";
        case ErrorCode::InvalidCommandId: return "Malformed command id";
        case ErrorCode::DuplicateId: return "Duplicate id";
        case ErrorCode::UnknownIntegration: return "No such integration";
        case ErrorCode::UnknownDevice: return "Device is not registered";
        case ErrorCode::DeviceRequired: return "This command needs a device_id";
        case ErrorCode::DeviceNotAllowed: return "System commands take device_id null";
        case ErrorCode::IntegrationMismatch: return "Device belongs to another integration";
        case ErrorCode::UnknownCommand: return "The device does not offer this command";
        case ErrorCode::UnknownParam: return "Unknown parameter for this command";
        case ErrorCode::DuplicateParam: return "Duplicate parameter";
        case ErrorCode::MissingParam: return "Required parameter is missing";
        case ErrorCode::InvalidParamType: return "Parameter has the wrong type";
        case ErrorCode::ParamOutOfRange: return "Parameter is out of range";
        case ErrorCode::InvalidEnumValue: return "Parameter is not one of the allowed options";
        case ErrorCode::TooManyParams: return "Too many parameters";
        case ErrorCode::InvalidTrigger: return "Trigger must be <button>:<event> with known names";
        case ErrorCode::DuplicateTrigger: return "Duplicate trigger";
        case ErrorCode::UnknownActivity: return "No such activity";
        case ErrorCode::MisplacedTransition: return "activate/off only as the last action of a binding";
        case ErrorCode::TooManyActions: return "Too many actions";
        case ErrorCode::TooManyActivities: return "Too many activities";
        case ErrorCode::TooManyDevices: return "Too many devices";
        case ErrorCode::CapacityExceeded: return "Configuration is too large";
        case ErrorCode::HoldNotAllowed:
            return "This command holds state until released (e.g. key_down); execute only takes momentary "
                   "commands such as key_tap. Holds are for bindings.";
        case ErrorCode::IntegrationDisabled: return "The device's integration is disabled";
        default: return errorCodeName(code);
    }
}

void putIssues(JsonArray out, const ValidationReport &report) {
    for (const auto &issue : report.issues()) {
        JsonObject d = out.add<JsonObject>();
        d["path"] = issue.path;
        d["code"] = errorCodeName(issue.code);
        d["message"] = issueMessage(issue.code);
    }
}

void sendIssues(http::Response &out, const ValidationReport &report, std::string_view message) {
    JsonDocument doc;
    putIssues(errorBody(doc, "validation_failed", message)["details"].to<JsonArray>(), report);
    sendJson(out, 422, doc);
}

void sendDetail(http::Response &out, std::string_view path, std::string_view code, std::string_view message) {
    JsonDocument doc;
    JsonObject d = errorBody(doc, "validation_failed", message)["details"].to<JsonArray>().add<JsonObject>();
    d["path"] = path;
    d["code"] = code;
    d["message"] = message;
    sendJson(out, 422, doc);
}

void sendCoreError(http::Response &out, ErrorCode code, std::string_view what) {
    const std::string subject(what);
    struct Mapping {
        int status;
        const char *code;
        std::string message;
    };
    const Mapping m = [&]() -> Mapping {
        switch (code) {
            case ErrorCode::NotFound:
            case ErrorCode::UnknownActivity:
            case ErrorCode::UnknownDevice:
            case ErrorCode::UnknownIntegration: return {404, "not_found", "No such " + subject};
            case ErrorCode::MalformedJson: return {400, "malformed_json", "Body is not valid JSON"};
            case ErrorCode::ValidationFailed: return {422, "validation_failed", "The " + subject + " is invalid"};
            case ErrorCode::RevisionConflict:
                return {409, "revision_conflict", "The " + subject + " changed; GET it again and reapply"};
            case ErrorCode::DeviceInUse: return {409, "device_in_use", "The device is still referenced"};
            case ErrorCode::Referenced: return {409, "activity_in_use", "The activity is still referenced"};
            case ErrorCode::LimitReached:
            case ErrorCode::TooManyActivities:
            case ErrorCode::TooManyDevices:
            case ErrorCode::CapacityExceeded: return {409, "limit_reached", "Too many of these already"};
            case ErrorCode::Busy: return {409, "busy", "Configuration is busy; retry shortly"};
            case ErrorCode::QueueFull: return {503, "busy", "The sequence queue is full; nothing was queued"};
            case ErrorCode::AdapterUnavailable:
                return {503, "integration_unavailable", "The integration is not connected"};
            case ErrorCode::NotImplemented: return {501, "not_implemented", "Not implemented yet"};
            case ErrorCode::ConfigBlocked:
                return {409, "config_blocked", "The saved configuration can't be used; restore a backup or reset"};
            case ErrorCode::StorageFailed:
                return {503, "storage_unavailable", "The configuration store can't be written; nothing changed"};
            case ErrorCode::IntegrationDisabled:
                return {503, "integration_unavailable", "The device's integration is disabled"};
            default: return {500, "internal_error", "Could not change the " + subject};
        }
    }();
    sendError(out, m.status, m.code, m.message);
}

void sendOutcome(http::Response &out, const config::Outcome &outcome, std::string_view what) {
    switch (outcome.code) {
        case ErrorCode::ValidationFailed:
            return sendIssues(out, outcome.issues, "The " + std::string(what) + " is invalid");
        case ErrorCode::RevisionConflict: {
            JsonDocument doc;
            errorBody(doc, "revision_conflict", "Changed since the revision you sent; GET it again and reapply")
                ["current_revision"] = outcome.currentRevision;
            return sendJson(out, 409, doc);
        }
        case ErrorCode::DeviceInUse:
        case ErrorCode::Referenced: {
            // Who still refers to it, as details, so the caller can fix them.
            JsonDocument doc;
            const bool device = outcome.code == ErrorCode::DeviceInUse;
            JsonArray details =
                errorBody(doc, device ? "device_in_use" : "activity_in_use",
                          "The " + std::string(what) + " is used elsewhere; remove those references first")
                    ["details"].to<JsonArray>();
            for (const auto &r : outcome.referrers) {
                JsonObject d = details.add<JsonObject>();
                d["path"] = (r.activityId.empty() ? std::string("/global_bindings") : "/activities/" + r.activityId) +
                            r.path;
                d["code"] = "referenced";
                d["message"] = r.activityId.empty() ? std::string("Referenced by the global bindings")
                                                    : "Referenced by activity " + r.activityId;
            }
            return sendJson(out, 409, doc);
        }
        default: return sendCoreError(out, outcome.code, what);
    }
}

std::string formatTimestamp(int64_t t) {
    if (t < 0) return {};
    // Civil date from days since 1970-01-01 (H. Hinnant's algorithm).
    const int64_t days = t / 86400;
    const int64_t secs = t % 86400;
    const int64_t z = days + 719468;
    const int64_t era = z / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", static_cast<int>(year), static_cast<int>(month),
             static_cast<int>(day), static_cast<int>(secs / 3600), static_cast<int>(secs / 60 % 60),
             static_cast<int>(secs % 60));
    return buf;
}

void putTimestamp(JsonObject obj, const char *key, int64_t t) {
    if (t < 0) {
        obj[key] = nullptr;
    } else {
        obj[key] = formatTimestamp(t);
    }
}

void putNullableId(JsonObject obj, const char *key, std::string_view id) {
    if (id.empty()) {
        obj[key] = nullptr;
    } else {
        obj[key] = id;
    }
}

JsonObject putIntegrationSummary(JsonArray list, const ApiContext &ctx, const IntegrationSpec *integration) {
    JsonObject i = list.add<JsonObject>();
    if (integration == nullptr) {
        i["id"] = kSystemIntegrationId;
        i["type"] = "system";
        i["state"] = "connected";
    } else {
        i["id"] = integration->id;
        i["type"] = integration->type;
        i["state"] = integration->enabled ? ctx.runtime.integrationState(integration->id) : "disabled";
    }
    return i;
}

void putConfigState(JsonObject out, const ApiContext &ctx) {
    const config::ConfigState state = ctx.config.state();
    out["state"] = config::configStateName(state);
    out["generation"] = state == config::ConfigState::Blocked ? ctx.config.lastLoad().generation
                                                               : ctx.config.generation();
    // Storage went away after a good load or commit: say so, not the stale
    // reason of that load.
    if (state == config::ConfigState::StorageUnavailable &&
        ctx.config.lastLoad().state != config::ConfigState::StorageUnavailable) {
        out["reason"] = "The configuration store is unavailable; the last saved config keeps running";
        return;
    }
    if (state == config::ConfigState::Blocked || state == config::ConfigState::StorageUnavailable) {
        const config::LoadReport &load = ctx.config.lastLoad();
        std::string reason;
        switch (load.reason) {
            case ErrorCode::UnsupportedSchema: reason = "Saved config is from newer firmware"; break;
            case ErrorCode::MigrationFailed: reason = "Saved config could not be migrated"; break;
            case ErrorCode::MalformedJson: reason = "Saved config is not valid JSON"; break;
            case ErrorCode::ValidationFailed: reason = "Saved config does not compile"; break;
            case ErrorCode::Corrupt: reason = "Saved config is damaged (no intact copy)"; break;
            case ErrorCode::StorageFailed: reason = "The configuration store can't be read or written"; break;
            default: reason = errorCodeName(load.reason); break;
        }
        if (!load.issues.ok()) {
            reason += " (" + std::string(errorCodeName(load.issues.issues()[0].code)) + " at " +
                      load.issues.issues()[0].path + ")";
        }
        out["reason"] = reason.substr(0, 120);
    }
}

const char *authMode(const ApiContext &ctx) { return ctx.info.simulatorNoAuth ? "simulator_no_auth" : "enforced"; }

bool readJson(const http::Request &request, http::Response &out, JsonDocument &doc, bool optional) {
    if (request.body.empty() && optional) {
        doc.to<JsonObject>();
        return true;
    }
    if (config::parseJson(request.body, doc, request.body.size(), config::kRequestNestingLimit) != ErrorCode::Ok) {
        sendError(out, 400, "malformed_json", "Body is not valid JSON (or nested too deeply)");
        return false;
    }
    return true;
}

bool checkObject(JsonVariantConst value, std::initializer_list<Field> fields, ValidationReport *report,
                 const std::string &path) {
    if (!value.is<JsonObjectConst>()) {
        report->add(ErrorCode::WrongType, path);
        return false;
    }
    auto at = [&](const char *name) { return path + "/" + jsonPointerToken(name); };
    const JsonObjectConst object = value.as<JsonObjectConst>();
    bool ok = true;
    for (JsonPairConst pair : object) {
        bool known = false;
        for (const auto &f : fields) known = known || pair.key() == f.name;
        if (!known) {
            report->add(ErrorCode::UnknownField, at(pair.key().c_str()));
            ok = false;
        }
    }
    for (const auto &f : fields) {
        JsonVariantConst v = object[f.name];
        if (v.isUnbound()) {
            if (f.required) {
                report->add(ErrorCode::Required, at(f.name));
                ok = false;
            }
            continue;
        }
        bool typed = false;
        switch (f.kind) {
            case Field::Kind::String: typed = v.is<const char *>(); break;
            case Field::Kind::Bool: typed = v.is<bool>(); break;
            case Field::Kind::Integer: typed = v.is<long long>() && !v.is<bool>(); break;
            case Field::Kind::Object: typed = v.is<JsonObjectConst>(); break;
        }
        if (!typed) {
            report->add(ErrorCode::WrongType, at(f.name));
            ok = false;
        } else if (f.kind == Field::Kind::String && f.maxLength > 0 &&
                   std::string_view(v.as<const char *>()).size() > f.maxLength) {
            report->add(ErrorCode::TooLong, at(f.name));
            ok = false;
        }
    }
    return ok;
}

bool queryRevision(const http::Request &request, http::Response &out, uint32_t *revision) {
    const std::string text = http::queryValue(request.query, "revision");
    uint64_t value = 0;
    bool ok = !text.empty() && text.size() <= 10 && text[0] != '0';
    for (char c : text) {
        if (c < '0' || c > '9') ok = false;
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    if (!ok || value == 0 || value > 0xFFFFFFFFu) {
        ValidationReport report;
        report.add(ErrorCode::Required, "/revision");
        sendIssues(out, report, "Send ?revision=N (the revision you last read)");
        return false;
    }
    *revision = static_cast<uint32_t>(value);
    return true;
}

const DeviceSpec *requireDevice(const ApiContext &ctx, std::string_view id, http::Response &out) {
    const DeviceSpec *device = ctx.config.findDevice(id);
    if (device == nullptr) sendError(out, 404, "not_found", "No such device");
    return device;
}

const ActivitySpec *requireActivity(const ApiContext &ctx, std::string_view id, http::Response &out) {
    const ActivitySpec *activity = ctx.config.findActivity(id);
    if (activity == nullptr) sendError(out, 404, "not_found", "No such activity");
    return activity;
}

bool runEngine(ApiContext &ctx, EngineRequest &request, http::Response &out) {
    if (ctx.runtime.run(request)) return true;
    sendError(out, 503, "busy", "The engine queue is full; retry shortly");
    return false;
}

}  // namespace hubbo::api
