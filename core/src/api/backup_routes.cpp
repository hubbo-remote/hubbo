// GET /admin/backup and POST /admin/restore: the non-secret configuration
// (integrations, devices, global bindings, activities) as one archive.
// Secrets stay in the SecretStore and are never in the archive.
//
// While the config is Blocked nothing runs, so the backup is the saved
// record's raw bytes, marked X-Hubbo-Backup: blocked-raw (design part 6:
// don't present a guess as data); restore takes that record back if this
// firmware can read it. StorageUnavailable: the running config if it came
// from a good load or commit; 503 if the load itself couldn't read storage.

#include "hubbo/config/json.h"
#include "hubbo/config/service.h"
#include "routes.h"
#include "support.h"

namespace hubbo::api {

namespace {

// A blocked-raw export: the saved record ({"format":"hubbo-config",...})
// rather than a Backup ({"format":"hubbo-backup",...}). Only the top-level
// "format" decides; anything else goes to the backup parser.
bool isConfigRecord(std::string_view body) {
    JsonDocument filter;
    filter["format"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter),
                        DeserializationOption::NestingLimit(16))) {
        return false;
    }
    JsonVariantConst format = doc["format"];
    return format.is<const char *>() && std::string_view(format.as<const char *>()) == "hubbo-config";
}

}  // namespace

void addBackupRoutes(RouteSink &sink, ApiContext &ctx) {
    sink.add("/api/v1/admin/backup", auth::Operation::DownloadBackup, [&ctx](auto &, auto &, auto &out) {
        config::BackupInfo info;
        info.createdAt = formatTimestamp(ctx.clock.unixSeconds());
        info.firmwareVersion = ctx.info.version;
        info.board = ctx.info.board;
        const std::string date = info.createdAt.empty() ? "undated" : info.createdAt.substr(0, 10);
        switch (ctx.config.state()) {
            case config::ConfigState::Ok:
            case config::ConfigState::Fresh: break;
            case config::ConfigState::StorageUnavailable:
                // The medium went away (or became untrusted) after a good
                // load or commit: what's running is the last acknowledged
                // save, so export it. Only a load that itself couldn't read
                // storage leaves nothing trustworthy: never an empty backup.
                if (ctx.config.lastLoad().state != config::ConfigState::StorageUnavailable) break;
                return sendError(out, 503, "storage_unavailable",
                                 "The configuration store can't be read right now; try again later");
            case config::ConfigState::Blocked: {
                // Nothing runs, so the runtime config is empty: export the
                // saved record exactly as stored, clearly marked, instead.
                std::string record;
                switch (ctx.config.blockedRecord(&record)) {
                    case ErrorCode::Ok: break;
                    case ErrorCode::StorageFailed:
                        return sendError(out, 503, "storage_unavailable",
                                         "The saved configuration can't be read right now; try again later");
                    case ErrorCode::Corrupt:
                        return sendError(out, 409, "config_blocked",
                                         "The saved configuration is damaged; no intact copy to back up");
                    default:
                        return sendError(out, 409, "config_blocked", "There is no saved configuration to back up");
                }
                out.status = 200;
                out.contentType = "application/octet-stream";
                out.body = std::move(record);
                out.headers.emplace_back("X-Hubbo-Backup", "blocked-raw");
                out.headers.emplace_back("Content-Disposition",
                                         "attachment; filename=\"hubbo-backup-BLOCKED-" + date + ".json\"");
                out.headers.emplace_back("Cache-Control", "no-store");
                return;
            }
        }
        out.json(200, config::serializeBackup(ctx.config.config(), info));
        out.headers.emplace_back("X-Hubbo-Backup", "config");
        out.headers.emplace_back("Content-Disposition", "attachment; filename=\"hubbo-backup-" + date + ".json\"");
        out.headers.emplace_back("Cache-Control", "no-store");
    });

    sink.add("/api/v1/admin/reset", auth::Operation::FactoryReset, [&ctx](auto &, auto &, auto &out) {
        const ConfigSpec empty;
        const ConfigSpec &defaults = ctx.factoryDefaults ? *ctx.factoryDefaults : empty;
        if (!forgetTokenForNewHaUrl(ctx, defaults, out)) return;
        const config::Outcome outcome = ctx.config.reset(defaults);
        reapplyHomeAssistant(ctx, true);
        if (!outcome.ok()) return sendOutcome(out, outcome, "configuration");
        JsonDocument doc;
        putConfigState(doc.to<JsonObject>(), ctx);
        sendJson(out, 200, doc);
    });

    sink.add("/api/v1/admin/restore", auth::Operation::RestoreBackup, [&ctx](auto &request, auto &, auto &out) {
        const std::string dryRunText = http::queryValue(request.query, "dry_run");
        const bool dryRun = dryRunText == "true" || dryRunText == "1";

        ConfigSpec restored;
        // A Backup, or a saved config record exported while blocked
        // (X-Hubbo-Backup: blocked-raw). The record is taken only if this
        // firmware can read it: same parser and migrations as boot.
        const bool rawRecord = isConfigRecord(request.body);
        const config::ParseResult parsed = rawRecord
                                               ? config::parseConfig(request.body, &restored)
                                               : config::parseBackup(request.body, &restored, nullptr, request.body.size());
        switch (parsed.code) {
            case ErrorCode::Ok: break;
            case ErrorCode::MalformedJson: return sendError(out, 400, "malformed_json", "The backup is not valid JSON");
            case ErrorCode::UnsupportedSchema:
                if (rawRecord) {
                    return sendDetail(out, "/schema_version", "unsupported_schema",
                                      "This saved configuration is from newer firmware; update first");
                }
                return sendDetail(out, "/format_version", "unsupported_schema",
                                  "This backup is from newer firmware; update first");
            case ErrorCode::MigrationFailed:
                return sendDetail(out, "/schema_version", "unsupported_schema",
                                  "This saved configuration can't be upgraded by this firmware");
            default:
                return sendIssues(out, parsed.report,
                                  rawRecord ? "Not a configuration this firmware can read" : "Not a valid Hubbo backup");
        }

        // Count before restoring: restore() takes the config by value.
        const size_t devices = restored.devices.size();
        const size_t activities = restored.activities.size();
        const size_t integrations = restored.integrations.size();
        bool homeAssistant = false;
        bool bluetooth = false;
        for (const auto &integration : restored.integrations) {
            homeAssistant = homeAssistant || integration.type == "home_assistant";
            bluetooth = bluetooth || integration.type == "ble_hid";
        }

        if (!dryRun && !forgetTokenForNewHaUrl(ctx, restored, out)) return;
        // One pipeline: a dry run is the real restore without the save.
        const config::Outcome outcome = ctx.config.restore(std::move(restored), dryRun);
        if (!dryRun) reapplyHomeAssistant(ctx, true);
        if (!outcome.ok()) return sendOutcome(out, outcome, "backup");

        JsonDocument doc;
        doc["dry_run"] = dryRun;
        doc["devices"] = devices;
        doc["activities"] = activities;
        doc["integrations"] = integrations;
        const int current = rawRecord ? config::kConfigFileVersion : config::kSchemaVersion;
        if (parsed.schemaVersion > 0 && parsed.schemaVersion < current) {
            doc["migrated_from"] = parsed.schemaVersion;
        } else {
            doc["migrated_from"] = nullptr;
        }
        // What the archive can't bring back; the admin re-enters or re-pairs.
        JsonArray followUps = doc["requires"].to<JsonArray>();
        if (homeAssistant) followUps.add("home_assistant_token");
        if (bluetooth) followUps.add("ble_pairing");
        if (ctx.credentials.tokens().empty()) followUps.add("api_tokens");
        doc["warnings"].to<JsonArray>();
        sendJson(out, 200, doc);
    });
}

}  // namespace hubbo::api
