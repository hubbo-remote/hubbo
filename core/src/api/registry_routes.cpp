// Devices, capabilities, command execution and integration summaries, plus
// the admin device registry.

#include <cstring>

#include "hubbo/config/json.h"
#include "hubbo/config/service.h"
#include "hubbo/domain/system.h"
#include "hubbo/ha/capabilities.h"
#include "hubbo/integrations/ha_client.h"
#include "routes.h"
#include "support.h"

namespace hubbo::api {

namespace {


const char *commandLabel(std::string_view id) {
    if (const char *label = ha::commandLabel(id)) return label;
    static constexpr struct {
        const char *id;
        const char *label;
    } kLabels[] = {
        {"key_down", "Key down"}, {"key_up", "Key up"}, {"key_tap", "Key tap"},
        {"wait", "Wait"},         {"activate", "Switch activity"}, {"off", "Off"},
    };
    for (const auto &l : kLabels) {
        if (id == l.id) return l.label;
    }
    return nullptr;
}

void putParam(JsonObject p, const ParamSpec &spec) {
    p["name"] = spec.name;
    p["type"] = paramTypeName(spec.type);
    p["required"] = spec.required;
    switch (spec.type) {
        case ParamType::Int:
            p["minimum"] = spec.minInt;
            p["maximum"] = spec.maxInt;
            p["step"] = 1;
            break;
        case ParamType::Number:
            p["minimum"] = spec.minNumber;
            p["maximum"] = spec.maxNumber;
            break;
        case ParamType::Enum: {
            JsonArray options = p["options"].to<JsonArray>();
            for (uint16_t i = 0; i < spec.optionCount; i++) options.add(spec.options[i]);
            break;
        }
        case ParamType::Bool:
        case ParamType::ActivityRef: break;
    }
}

// `device`: an HA device whose integration services carry HA's own labels.
void putCommands(JsonArray out, CommandCatalog catalog, const DeviceSpec *device = nullptr) {
    for (uint16_t i = 0; i < catalog.count; i++) {
        const CommandSpec &c = catalog.items[i];
        JsonObject cmd = out.add<JsonObject>();
        cmd["command_id"] = c.id;
        const char *label = commandLabel(c.id);
        if (label == nullptr && device != nullptr && device->haCapabilities) {
            for (const ha::SnapshotService &s : device->haCapabilities->services) {
                if (s.service == c.id && !s.label.empty()) {
                    label = s.label.c_str();
                    break;
                }
            }
        }
        cmd["label"] = label != nullptr ? label : c.id;
        cmd["effect"] = commandEffectName(c.effect);
        JsonArray params = cmd["params"].to<JsonArray>();
        for (uint8_t p = 0; p < c.paramCount; p++) putParam(params.add<JsonObject>(), c.params[p]);
    }
}

CommandCatalog catalogFor(const ApiContext &ctx, const DeviceSpec &device) {
    const AdapterSet &adapters = ctx.runtime.adapters();
    const int index = adapters.find(device.integrationId);
    return index < 0 ? CommandCatalog{} : adapters.at(static_cast<size_t>(index))->commandsFor(device);
}

void putDevice(JsonObject out, const ApiContext &ctx, const DeviceSpec &device) {
    config::encodeDevice(device, out, false);  // the snapshot stays on the hub
    out["availability"] = ctx.runtime.deviceAvailability(device);
}

const char *execStatus(ExecStatus status) {
    switch (status) {
        case ExecStatus::Accepted: return "accepted";
        case ExecStatus::Timeout: return "timeout";
        default: return "failed";
    }
}

// HA providers must name one specific entity (no "all", lists or garbage).
bool checkProvider(const ProviderSpec &provider, http::Response &out, const char *path) {
    if (provider.type == "home_assistant" && !ha::isValidEntityId(provider.entityId)) {
        sendDetail(out, path, "pattern", "entity_id must be one specific <domain>.<object_id> entity");
        return false;
    }
    return true;
}

// An HA provider's snapshot, from that entity's finished details (POST/GET
// /admin/integrations/ha_main/entities/{entity_id}/details). Fills the
// provider's unique_id from them. One device per entity: another device on
// it (also one the entity was renamed from) is 422 duplicate_id; `selfId` is
// the device being re-linked. False after sending 503 / 409 / 422.
bool snapshotFor(ApiContext &ctx, ProviderSpec *provider, std::string_view selfId, ha::CapabilitySnapshot *out,
                 http::Response &res) {
    if (ctx.homeAssistant == nullptr) {
        sendError(res, 503, "integration_unavailable", "This hub has no Home Assistant client");
        return false;
    }
    const EntityDetails *d = completedDetails(ctx, provider->entityId);
    if (d == nullptr) {
        sendError(res, 409, "details_required",
                  "Load this entity's details first (POST /api/v1/admin/integrations/ha_main/entities/" +
                      provider->entityId + "/details)");
        return false;
    }
    if (d->commandIds.empty()) {
        sendDetail(res, "/provider/entity_id", "unknown_device", "Hubbo offers no commands for this entity");
        return false;
    }
    const DeviceSpec *existing = registeredDevice(ctx, d->entityId, d->uniqueId, d->platform);
    if (existing != nullptr && existing->id != selfId) {
        sendDetail(res, "/provider/entity_id", "duplicate_id",
                   "Already registered as " + existing->id + "; re-link that device instead");
        return false;
    }
    provider->uniqueId = d->uniqueId;
    *out = d->snapshot;
    return true;
}

}  // namespace

void addRegistryRoutes(RouteSink &sink, ApiContext &ctx) {
    sink.add("/api/v1/devices", auth::Operation::ListDevices, [&ctx](auto &, auto &, auto &out) {
        JsonDocument doc;
        JsonArray devices = doc["devices"].to<JsonArray>();
        for (const auto &d : ctx.config.config().devices) putDevice(devices.add<JsonObject>(), ctx, d);
        sendJson(out, 200, doc);
    });

    sink.add("/api/v1/devices/{device_id}", auth::Operation::GetDevice, [&ctx](auto &, auto &p, auto &out) {
        const DeviceSpec *device = requireDevice(ctx, p[0], out);
        if (device == nullptr) return;
        JsonDocument doc;
        putDevice(doc.to<JsonObject>(), ctx, *device);
        sendJson(out, 200, doc);
    });

    sink.add("/api/v1/devices/{device_id}/capabilities", auth::Operation::GetDeviceCapabilities,
             [&ctx](auto &, auto &p, auto &out) {
                 const DeviceSpec *device = requireDevice(ctx, p[0], out);
                 if (device == nullptr) return;
                 JsonDocument doc;
                 doc["device_id"] = device->id;
                 doc["integration_id"] = device->integrationId;
                 putCommands(doc["commands"].to<JsonArray>(), catalogFor(ctx, *device), device);
                 sendJson(out, 200, doc);
             });

    sink.add("/api/v1/devices/{device_id}/commands/{command_id}/execute", auth::Operation::ExecuteCommand,
             [&ctx](auto &request, auto &p, auto &out) {
                 JsonDocument doc;
                 if (!readJson(request, out, doc)) return;
                 ValidationReport report;
                 std::vector<ParamInput> params;
                 if (checkObject(doc.as<JsonVariantConst>(), {{"params", Field::Kind::Object, true}}, &report)) {
                     config::decodeParams(doc["params"], &params, &report, "/params");
                 }
                 if (!report.ok()) return sendIssues(out, report, "Send {params: {...}}");
                 // Checked first so a missing device isn't reported as a missing command.
                 if (requireDevice(ctx, p[0], out) == nullptr) return;
                 CompiledCommand command;
                 const config::Outcome compiled = ctx.config.compileCommand(p[0], p[1], params, &command);
                 if (!compiled.ok()) return sendOutcome(out, compiled, "command");
                 EngineRequest run;
                 run.execute(command);
                 if (!runEngine(ctx, run, out)) return;
                 const CommandResult &result = run.command();
                 JsonDocument body;
                 body["status"] = execStatus(result.status);
                 body["device_id"] = p[0];
                 body["command_id"] = p[1];
                 body["elapsed_ms"] = result.elapsedMs;
                 if (result.error != ErrorCode::Ok) body["message"] = errorCodeName(result.error);
                 sendJson(out, 200, body);
             });

    sink.add("/api/v1/integrations", auth::Operation::ListIntegrations, [&ctx](auto &, auto &, auto &out) {
        const ConfigSpec &config = ctx.config.config();
        std::vector<size_t> deviceCounts(config.integrations.size(), 0);
        for (const auto &d : config.devices) {
            for (size_t i = 0; i < config.integrations.size(); i++) {
                if (config.integrations[i].id == d.integrationId) deviceCounts[i]++;
            }
        }
        JsonDocument doc;
        JsonArray list = doc["integrations"].to<JsonArray>();
        JsonObject system = putIntegrationSummary(list, ctx, nullptr);
        system["name"] = "System";
        system["enabled"] = true;
        system["device_count"] = 0;
        putCommands(system["commands"].to<JsonArray>(), systemCatalog());
        for (size_t n = 0; n < config.integrations.size(); n++) {
            const IntegrationSpec &integration = config.integrations[n];
            if (integration.id == kSystemIntegrationId) continue;
            JsonObject i = putIntegrationSummary(list, ctx, &integration);
            i["name"] = integration.name;
            i["enabled"] = integration.enabled;
            const std::string detail = ctx.runtime.integrationDetail(integration.id);
            if (!detail.empty()) i["detail"] = detail;
            i["device_count"] = deviceCounts[n];
        }
        sendJson(out, 200, doc);
    });

    // ---- admin device registry ----
    sink.add("/api/v1/admin/devices", auth::Operation::RegisterDevice, [&ctx](auto &request, auto &, auto &out) {
        JsonDocument doc;
        if (!readJson(request, out, doc)) return;
        ValidationReport report;
        DeviceSpec device;
        if (!config::decodeDevice(doc.as<JsonVariantConst>(), config::DeviceShape::Register, &device, &report)) {
            return sendIssues(out, report, "The device is invalid");
        }
        if (!checkProvider(device.provider, out, "/provider/entity_id")) return;
        if (device.provider.type == kHomeAssistantType) {
            ha::CapabilitySnapshot snapshot;
            if (!snapshotFor(ctx, &device.provider, {}, &snapshot, out)) return;
            device.haCapabilities = std::move(snapshot);
        }
        const config::Outcome outcome = ctx.config.createDevice(std::move(device));
        if (!outcome.ok()) return sendOutcome(out, outcome, "device");
        JsonDocument body;
        putDevice(body.to<JsonObject>(), ctx, *ctx.config.findDevice(outcome.id));
        out.headers.emplace_back("Location", "/api/v1/devices/" + outcome.id);
        sendJson(out, 201, body);
    });

    sink.add("/api/v1/admin/devices/{device_id}", auth::Operation::UpdateDevice,
             [&ctx](auto &request, auto &p, auto &out) {
                 JsonDocument doc;
                 if (!readJson(request, out, doc)) return;
                 ValidationReport report;
                 config::DeviceUpdate update;
                 if (!config::decodeDeviceUpdate(doc.as<JsonVariantConst>(), &update, &report)) {
                     return sendIssues(out, report, "The device update is invalid");
                 }
                 if (update.provider && !checkProvider(*update.provider, out, "/provider/entity_id")) return;
                 if (update.provider && update.provider->type == kHomeAssistantType) {  // re-link: refresh
                     ha::CapabilitySnapshot snapshot;
                     if (!snapshotFor(ctx, &*update.provider, p[0], &snapshot, out)) return;
                     update.haCapabilities = std::move(snapshot);
                 }
                 const config::Outcome outcome = ctx.config.updateDevice(p[0], update);
                 if (!outcome.ok()) return sendOutcome(out, outcome, "device");
                 JsonDocument body;
                 putDevice(body.to<JsonObject>(), ctx, *ctx.config.findDevice(outcome.id));
                 sendJson(out, 200, body);
             });

    sink.add("/api/v1/admin/devices/{device_id}", auth::Operation::DeleteDevice,
             [&ctx](auto &request, auto &p, auto &out) {
                 // 404 before 422: a missing device beats a missing ?revision.
                 if (requireDevice(ctx, p[0], out) == nullptr) return;
                 uint32_t revision = 0;
                 if (!queryRevision(request, out, &revision)) return;
                 const config::Outcome outcome = ctx.config.deleteDevice(p[0], revision);
                 if (!outcome.ok()) return sendOutcome(out, outcome, "device");
                 sendEmpty(out, 204);
             });
}

}  // namespace hubbo::api
