// Smart buttons (docs/design/smart-buttons.md): the four slots, each a
// device or null, and the actions the hub derives from each.

#include "hubbo/config/json.h"
#include "hubbo/config/service.h"
#include "hubbo/domain/ids.h"
#include "hubbo/engine/smart_actions.h"
#include "hubbo/http/route.h"
#include "routes.h"
#include "support.h"

namespace hubbo::api {

namespace {

// A slot's actions with `device` (smartSlotActions), or none for a null slot.
SmartSlotActions actionsFor(ApiContext &ctx, const DeviceSpec *device) {
    return device ? smartSlotActions(*device, catalogFor(ctx, *device)) : SmartSlotActions{};
}

// {slots, actions, revision}: `actions` per slot from its device.
void sendSmartButtons(http::Response &out, ApiContext &ctx) {
    const SmartButtonsSpec &spec = ctx.config.config().smartButtons;
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    config::encodeSmartButtons(spec, root);
    JsonObject actions = root["actions"].to<JsonObject>();
    for (size_t i = 0; i < kSmartSlotCount; i++) {
        const auto slot = static_cast<SmartSlot>(i);
        const DeviceSpec *device = spec.at(slot) ? ctx.config.findDevice(*spec.at(slot)) : nullptr;
        config::encodeSmartActions(actionsFor(ctx, device), actions[smartSlotName(slot)].to<JsonObject>());
    }
    sendJson(out, 200, doc);
}

}  // namespace

void addSmartButtonRoutes(RouteSink &sink, ApiContext &ctx) {
    sink.add("/api/v1/smart-buttons", auth::Operation::GetSmartButtons,
             [&ctx](auto &, auto &, auto &out) { sendSmartButtons(out, ctx); });

    sink.add("/api/v1/smart-buttons", auth::Operation::ReplaceSmartButtons, [&ctx](auto &request, auto &, auto &out) {
        JsonDocument doc;
        if (!readJson(request, out, doc)) return;
        ValidationReport report;
        SmartButtonsSpec spec;
        if (!config::decodeSmartButtons(doc.as<JsonVariantConst>(), &spec, &report)) {
            return sendIssues(out, report, "The smart buttons are invalid");
        }
        doc.clear();
        const uint32_t base = spec.revision;
        const config::Outcome outcome = ctx.config.replaceSmartButtons(std::move(spec), base);
        if (!outcome.ok()) return sendOutcome(out, outcome, "smart buttons");
        sendSmartButtons(out, ctx);
    });

    // A preview: what a slot would do with `device_id`, nothing saved.
    sink.add("/api/v1/smart-buttons/actions", auth::Operation::GetSmartButtonActions,
             [&ctx](auto &request, auto &, auto &out) {
                 // The spec's DeviceId (^dev_[a-z0-9_]{1,28}$): missing or
                 // malformed is 422, a well-formed unknown id 404.
                 const std::string id = http::queryValue(request.query, "device_id");
                 if (id.size() <= 4 || id.compare(0, 4, "dev_") != 0 || !isValidId(id)) {
                     ValidationReport report;
                     report.add(id.empty() ? ErrorCode::Required : ErrorCode::InvalidId, "/device_id");
                     return sendIssues(out, report, "Send ?device_id=<the device>");
                 }
                 const DeviceSpec *device = requireDevice(ctx, id, out);
                 if (device == nullptr) return;
                 JsonDocument doc;
                 config::encodeSmartActions(actionsFor(ctx, device), doc.to<JsonObject>());
                 sendJson(out, 200, doc);
             });
}

}  // namespace hubbo::api
