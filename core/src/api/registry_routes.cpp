// Devices, capabilities, command execution and integration summaries, plus
// the admin device registry.

#include <cstring>
#include <memory>
#include <optional>

#include "hubbo/config/json.h"
#include "hubbo/config/profile.h"
#include "hubbo/config/service.h"
#include "hubbo/domain/system.h"
#include "hubbo/ha/capabilities.h"
#include "hubbo/ha/snapshot_json.h"
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
        {"press", "Press"},
        {"press_down", "Press down"},
        {"press_up", "Press up"},
        {"wait_connected", "Wait until connected"},
        {"key_down", "Key down"},
        {"key_up", "Key up"},
        {"key_tap", "Key tap"},
        {"usage_down", "Usage down"},
        {"usage_up", "Usage up"},
        {"usage_tap", "Usage tap"},
        {"wait", "Wait"},
        {"activate", "Switch activity"},
        {"off", "Off"},
    };
    for (const auto &l : kLabels) {
        if (id == l.id) return l.label;
    }
    return nullptr;
}

// ParamSpec in the spec's shape, with its display metadata (label, unit,
// step, description, default) when the integration gives it.
void putParam(JsonObject p, const ParamSpec &spec) {
    p["name"] = spec.name;
    if (spec.label != nullptr) p["label"] = spec.label;
    p["type"] = paramTypeName(spec.type);
    p["required"] = spec.required;
    switch (spec.type) {
        case ParamType::Int:
            p["minimum"] = spec.minInt;
            p["maximum"] = spec.maxInt;
            p["step"] = spec.step >= 1.0f ? spec.step : 1.0f;  // an int steps by 1 unless its spec says more
            break;
        case ParamType::Number:
            p["minimum"] = spec.minNumber;
            p["maximum"] = spec.maxNumber;
            if (spec.step > 0.0f) p["step"] = spec.step;
            break;
        case ParamType::Enum: {
            JsonArray options = p["options"].to<JsonArray>();
            for (uint16_t i = 0; i < spec.optionCount; i++) options.add(spec.options[i]);
            break;
        }
        case ParamType::Bool:
        case ParamType::ActivityRef: break;
    }
    if (spec.unit != nullptr) p["unit"] = spec.unit;
    if (spec.description != nullptr) p["description"] = spec.description;
    if (!spec.hasDefault) return;
    switch (spec.type) {
        case ParamType::Int: p["default"] = spec.defaultValue.integer; break;
        case ParamType::Number: p["default"] = spec.defaultValue.number; break;
        case ParamType::Bool: p["default"] = spec.defaultValue.asBool(); break;
        case ParamType::Enum:
            if (spec.defaultValue.asOption() < spec.optionCount) p["default"] = spec.options[spec.defaultValue.asOption()];
            break;
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
        if (c.group != nullptr) cmd["group"] = c.group;
        JsonArray params = cmd["params"].to<JsonArray>();
        for (uint8_t p = 0; p < c.paramCount; p++) putParam(params.add<JsonObject>(), c.params[p]);
        // The Home Assistant action's choices: the snapshot's custom services, with their field hints.
        if (device != nullptr && device->haCapabilities && std::string_view(c.id) == ha::kCallServiceCommandId) {
            ha::encodeCustomServices(device->haCapabilities->customServices, cmd["services"].to<JsonArray>());
        }
    }
}

CommandCatalog catalogFor(const ApiContext &ctx, const DeviceSpec &device) {
    const AdapterSet &adapters = ctx.runtime.adapters();
    const int index = adapters.find(device.integrationId);
    return index < 0 ? CommandCatalog{} : adapters.at(static_cast<size_t>(index))->commandsFor(device);
}

// `detail`: Full (GET /devices/{id} and the admin answers) or Summary (the list).
void putDevice(JsonObject out, const ApiContext &ctx, const DeviceSpec &device,
               config::DeviceDetail detail = config::DeviceDetail::Full) {
    config::encodeDevice(device, out, detail);  // never the HA snapshot
    out["platform"] = device.provider.type;
    out["availability"] = ctx.runtime.deviceAvailability(device);
    if (device.provider.type == kBleHidType && ctx.bleHosts != nullptr) {
        putBluetoothStatus(out["bluetooth"].to<JsonObject>(), ctx.bleHosts->status(device.id));
    }
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
    // The Home Assistant action's hints share one budget across the config:
    // take what's left (other devices'), so adding a device never fails on them.
    const size_t used = ha::configHintBytes(ctx.config.config().devices, selfId);
    ha::trimCustomServices(&out->customServices, used < ha::kMaxConfigHintBytes ? ha::kMaxConfigHintBytes - used : 0);
    return true;
}

// {profile_id, name?, kind?}: a device copied from a built-in profile, or null
// after sending 422. Built straight onto the heap (no DeviceSpec temporary).
std::unique_ptr<DeviceSpec> deviceFromBody(JsonVariantConst body, http::Response &out) {
    ValidationReport report;
    if (!checkObject(body,
                     {{"profile_id", Field::Kind::String, true, limits::kMaxProfileIdLength},
                      {"name", Field::Kind::String, false, limits::kMaxNameLength},
                      {"kind", Field::Kind::String, false, limits::kMaxTypeLength}},
                     &report)) {
        sendIssues(out, report, "Send {profile_id, name?, kind?}");
        return nullptr;
    }
    const std::string_view id = body["profile_id"].as<const char *>();
    const config::BuiltinProfile *profile = config::builtinProfiles().find(id);
    if (profile == nullptr) {
        sendDetail(out, "/profile_id", "unknown_profile", "There's no profile " + std::string(id));
        return nullptr;
    }
    std::string name = body["name"] | std::string();
    std::string kind = body["kind"] | std::string();
    return std::unique_ptr<DeviceSpec>(
        new DeviceSpec(config::deviceFromProfile(*profile, std::move(name), std::move(kind))));
}

// POST /admin/devices: the new device from the body (a profile, or an
// HA entity with its loaded details), or null after sending the error.
std::unique_ptr<DeviceSpec> newDevice(ApiContext &ctx, const http::Request &request, http::Response &out) {
    JsonDocument doc;
    if (!readJson(request, out, doc)) return nullptr;
    if (!doc["profile_id"].isNull()) return deviceFromBody(doc.as<JsonVariantConst>(), out);
    // Bluetooth devices come from a profile; a client never sends one
    // (checked first: the decoder would ask for the profile's fields).
    // Any other device's decoder reports profile fields as unknown_field.
    if (doc["provider"]["type"] == kBleHidType) {
        sendDetail(out, "/profile_id", "required", "Add a Bluetooth device from a profile");
        return nullptr;
    }
    auto device = std::make_unique<DeviceSpec>();
    ValidationReport report;
    if (!config::decodeDevice(doc.as<JsonVariantConst>(), config::DeviceShape::Register, device.get(), &report)) {
        sendIssues(out, report, "The device is invalid");
        return nullptr;
    }
    if (!checkProvider(device->provider, out, "/provider/entity_id")) return nullptr;
    if (!snapshotFor(ctx, &device->provider, {}, &device->haCapabilities.emplace(), out)) return nullptr;
    return device;
}

// PUT /admin/devices/{id}: the update from the body (an HA re-link
// refreshes the snapshot), or null after sending the error.
std::unique_ptr<config::DeviceUpdate> deviceUpdate(ApiContext &ctx, const http::Request &request,
                                                   std::string_view id, http::Response &out) {
    JsonDocument doc;
    if (!readJson(request, out, doc)) return nullptr;
    auto update = std::make_unique<config::DeviceUpdate>();
    ValidationReport report;
    if (!config::decodeDeviceUpdate(doc.as<JsonVariantConst>(), update.get(), &report)) {
        sendIssues(out, report, "The device update is invalid");
        return nullptr;
    }
    if (update->provider && !checkProvider(*update->provider, out, "/provider/entity_id")) return nullptr;
    if (update->provider && update->provider->type == kHomeAssistantType &&  // re-link: refresh
        !snapshotFor(ctx, &*update->provider, id, &update->haCapabilities.emplace(), out)) {
        return nullptr;
    }
    return update;
}

// POST .../execute: {params}, or false after sending 422.
// The execute body: {params} and, for call_service (a Home Assistant
// action) only and always there, its `call`.
bool execParams(const http::Request &request, std::string_view commandId, std::vector<ParamInput> *params,
                std::optional<HaServiceCallSpec> *call, http::Response &out) {
    JsonDocument doc;
    if (!readJson(request, out, doc)) return false;
    ValidationReport report;
    const bool custom = commandId == ha::kCallServiceCommandId;
    if (checkObject(doc.as<JsonVariantConst>(),
                    {{"params", Field::Kind::Object, true}, {"call", Field::Kind::Object, custom}}, &report)) {
        config::decodeParams(doc["params"], params, &report, "/params");
        if (!doc["call"].isUnbound()) {
            if (!custom) {
                report.add(ErrorCode::UnknownField, "/call");
            } else {
                config::decodeHaServiceCall(doc["call"], &call->emplace(), &report, "/call");
            }
        }
    }
    if (report.ok()) return true;
    sendIssues(out, report, custom ? "Send {params: {}, call: {service, data}}" : "Send {params: {...}}");
    return false;
}

// A device write's answer: the saved device, or the save's error.
void sendSavedDevice(const ApiContext &ctx, const config::Outcome &outcome, int status, http::Response &out) {
    if (!outcome.ok()) return sendOutcome(out, outcome, "device");
    JsonDocument body;
    putDevice(body.to<JsonObject>(), ctx, *ctx.config.findDevice(outcome.id));
    if (status == 201) out.headers.emplace_back("Location", "/api/v1/devices/" + outcome.id);
    sendJson(out, status, body);
}

}  // namespace

void addRegistryRoutes(RouteSink &sink, ApiContext &ctx) {
    sink.add("/api/v1/devices", auth::Operation::ListDevices, [&ctx](auto &, auto &, auto &out) {
        JsonDocument doc;
        JsonArray devices = doc["devices"].to<JsonArray>();
        for (const auto &d : ctx.config.config().devices) {
            putDevice(devices.add<JsonObject>(), ctx, d, config::DeviceDetail::Summary);
        }
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
                 std::vector<ParamInput> params;
                 std::optional<HaServiceCallSpec> call;
                 if (!execParams(request, p[1], &params, &call, out)) return;
                 // Checked first so a missing device isn't reported as a missing command.
                 if (requireDevice(ctx, p[0], out) == nullptr) return;
                 const auto run = std::make_unique<EngineRequest>();
                 {
                     CompiledCommand command;
                     const config::Outcome compiled =
                         ctx.config.compileCommand(p[0], p[1], params, &command, call ? &*call : nullptr);
                     if (!compiled.ok()) return sendOutcome(out, compiled, "command");
                     run->execute(command);
                 }
                 if (!runEngine(ctx, *run, out)) return;
                 const CommandResult &result = run->command();
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
            i["device_count"] = deviceCounts[n];
        }
        sendJson(out, 200, doc);
    });

    // ---- admin device registry ----
    sink.add("/api/v1/admin/devices", auth::Operation::RegisterDevice, [&ctx](auto &request, auto &, auto &out) {
        std::unique_ptr<DeviceSpec> device = newDevice(ctx, request, out);
        if (!device) return;
        const config::Outcome outcome = ctx.config.createDevice(std::move(*device));
        // The Bluetooth limit: TooManyDevices at the new device's /integration_id
        // (createDevice reroots /devices/N/...; the 64-device cap is at /devices).
        if (outcome.issues.has(ErrorCode::TooManyDevices, "/integration_id")) {
            return sendDetail(out, "/integration_id", "too_many_devices",
                              "You can add up to " + std::to_string(limits::kMaxBleDevices) + " Bluetooth devices");
        }
        sendSavedDevice(ctx, outcome, 201, out);
    });

    sink.add("/api/v1/admin/devices/{device_id}", auth::Operation::UpdateDevice,
             [&ctx](auto &request, auto &p, auto &out) {
                 std::unique_ptr<config::DeviceUpdate> update = deviceUpdate(ctx, request, p[0], out);
                 if (!update) return;
                 const config::Outcome outcome = ctx.config.updateDevice(p[0], *update);
                 update.reset();
                 sendSavedDevice(ctx, outcome, 200, out);
             });

    sink.add("/api/v1/admin/devices/{device_id}", auth::Operation::DeleteDevice,
             [&ctx](auto &request, auto &p, auto &out) {
                 // 404 before 422: a missing device beats a missing ?revision.
                 if (requireDevice(ctx, p[0], out) == nullptr) return;
                 uint32_t revision = 0;
                 if (!queryRevision(request, out, &revision)) return;
                 // Its bond goes with it: the hosts reconcile with every saved config.
                 const config::Outcome outcome = ctx.config.deleteDevice(p[0], revision);
                 if (!outcome.ok()) return sendOutcome(out, outcome, "device");
                 sendEmpty(out, 204);
             });
}

}  // namespace hubbo::api
