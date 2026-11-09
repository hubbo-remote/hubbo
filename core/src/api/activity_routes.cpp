// Activities, global bindings and activity transitions.

#include "hubbo/config/json.h"
#include "hubbo/config/service.h"
#include "routes.h"
#include "support.h"

namespace hubbo::api {

namespace {

void sendActivity(http::Response &out, int status, const ActivitySpec &activity) {
    JsonDocument doc;
    config::encodeActivity(activity, doc.to<JsonObject>());
    sendJson(out, status, doc);
}

void putPhase(JsonObject out, const TransitionPhase &phase) {
    out["status"] = phaseStatusName(phase.status);
    out["queued_actions"] = phase.status == PhaseStatus::Queued ? phase.actions : 0;
    if (phase.status == PhaseStatus::Rejected) out["reason"] = errorCodeName(phase.reason);
}

// Runs an activate/off request and answers with what happened, phase by
// phase. A transition that switched is final (200), even if its sequence was
// rejected; only a request that never reached the engine is a retryable 503.
void transition(ApiContext &ctx, EngineRequest &request, http::Response &out) {
    // A blocked hub runs no configuration: say so rather than 404 an id the
    // saved config may well have. (storage_unavailable doesn't stop
    // switching; the active activity just isn't saved.)
    if (ctx.config.state() == config::ConfigState::Blocked) return sendCoreError(out, ErrorCode::ConfigBlocked, "");
    if (!runEngine(ctx, request, out)) return;
    const TransitionResult &t = request.transition();
    // Off with nothing active is a no-op (switched == false), not an error.
    if (!t.switched && t.error != ErrorCode::NoActiveActivity) {
        return sendCoreError(out, t.error, "activity");
    }
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    root["switched"] = t.switched;
    putNullableId(root, "active_activity_id", t.activeActivityId.view());
    putNullableId(root, "previous_activity_id", t.previousActivityId.view());
    root["generation"] = t.generation;
    putPhase(root["startup"].to<JsonObject>(), t.startup);
    putPhase(root["shutdown"].to<JsonObject>(), t.shutdown);
    sendJson(out, 200, doc);
}

}  // namespace

void addActivityRoutes(RouteSink &sink, ApiContext &ctx) {
    sink.add("/api/v1/activities", auth::Operation::ListActivities, [&ctx](auto &, auto &, auto &out) {
        JsonDocument doc;
        JsonArray list = doc["activities"].to<JsonArray>();
        for (const auto &a : ctx.config.config().activities) config::encodeActivity(a, list.add<JsonObject>());
        putNullableId(doc.as<JsonObject>(), "active_activity_id", ctx.runtime.snapshot().activeActivityId);
        sendJson(out, 200, doc);
    });

    sink.add("/api/v1/activities", auth::Operation::CreateActivity, [&ctx](auto &request, auto &, auto &out) {
        JsonDocument doc;
        if (!readJson(request, out, doc)) return;
        ValidationReport report;
        ActivitySpec activity;
        if (!config::decodeActivity(doc.as<JsonVariantConst>(), config::ActivityShape::Create, &activity, &report)) {
            return sendIssues(out, report, "The activity is invalid");
        }
        const config::Outcome outcome = ctx.config.createActivity(std::move(activity));
        if (!outcome.ok()) return sendOutcome(out, outcome, "activity");
        out.headers.emplace_back("Location", "/api/v1/activities/" + outcome.id);
        sendActivity(out, 201, *ctx.config.findActivity(outcome.id));
    });

    sink.add("/api/v1/activities/validate", auth::Operation::ValidateActivity,
             [&ctx](auto &request, auto &, auto &out) {
                 JsonDocument doc;
                 if (!readJson(request, out, doc)) return;
                 ValidationReport report;
                 ActivitySpec activity;
                 if (config::decodeActivity(doc.as<JsonVariantConst>(), config::ActivityShape::Validate, &activity,
                                            &report)) {
                     report = ctx.config.validateActivity(activity);
                 }
                 JsonDocument body;
                 body["valid"] = report.ok();
                 putIssues(body["errors"].to<JsonArray>(), report);
                 sendJson(out, 200, body);
             });

    sink.add("/api/v1/activities/off", auth::Operation::ActivitiesOff, [&ctx](auto &, auto &, auto &out) {
        EngineRequest request;
        request.off();
        transition(ctx, request, out);
    });

    sink.add("/api/v1/activities/{activity_id}", auth::Operation::GetActivity, [&ctx](auto &, auto &p, auto &out) {
        if (const ActivitySpec *activity = requireActivity(ctx, p[0], out)) sendActivity(out, 200, *activity);
    });

    sink.add("/api/v1/activities/{activity_id}", auth::Operation::ReplaceActivity,
             [&ctx](auto &request, auto &p, auto &out) {
                 JsonDocument doc;
                 if (!readJson(request, out, doc)) return;
                 ValidationReport report;
                 ActivitySpec activity;
                 if (!config::decodeActivity(doc.as<JsonVariantConst>(), config::ActivityShape::Replace, &activity,
                                             &report)) {
                     return sendIssues(out, report, "The activity is invalid");
                 }
                 const uint32_t base = activity.revision;
                 const config::Outcome outcome = ctx.config.replaceActivity(p[0], std::move(activity), base);
                 if (!outcome.ok()) return sendOutcome(out, outcome, "activity");
                 sendActivity(out, 200, *ctx.config.findActivity(outcome.id));
             });

    sink.add("/api/v1/activities/{activity_id}", auth::Operation::DeleteActivity,
             [&ctx](auto &request, auto &p, auto &out) {
                 // 404 before 422: a missing activity beats a missing ?revision.
                 if (requireActivity(ctx, p[0], out) == nullptr) return;
                 uint32_t revision = 0;
                 if (!queryRevision(request, out, &revision)) return;
                 const config::Outcome outcome = ctx.config.deleteActivity(p[0], revision);
                 if (!outcome.ok()) return sendOutcome(out, outcome, "activity");
                 sendEmpty(out, 204);
             });

    sink.add("/api/v1/activities/{activity_id}/activate", auth::Operation::ActivateActivity,
             [&ctx](auto &, auto &p, auto &out) {
                 EngineRequest request;
                 request.activate(p[0]);
                 transition(ctx, request, out);
             });

    sink.add("/api/v1/global-bindings", auth::Operation::GetGlobalBindings, [&ctx](auto &, auto &, auto &out) {
        JsonDocument doc;
        const ConfigSpec &config = ctx.config.config();
        config::encodeGlobalBindings(config.globalBindings, config.globalBindingsRevision, doc.to<JsonObject>());
        sendJson(out, 200, doc);
    });

    sink.add("/api/v1/global-bindings", auth::Operation::ReplaceGlobalBindings,
             [&ctx](auto &request, auto &, auto &out) {
                 JsonDocument doc;
                 if (!readJson(request, out, doc)) return;
                 ValidationReport report;
                 std::vector<BindingSpec> bindings;
                 uint32_t revision = 0;
                 if (!config::decodeGlobalBindings(doc.as<JsonVariantConst>(), &bindings, &revision, &report)) {
                     return sendIssues(out, report, "The global bindings are invalid");
                 }
                 const config::Outcome outcome = ctx.config.replaceGlobalBindings(std::move(bindings), revision);
                 if (!outcome.ok()) return sendOutcome(out, outcome, "global bindings");
                 JsonDocument body;
                 const ConfigSpec &config = ctx.config.config();
                 config::encodeGlobalBindings(config.globalBindings, config.globalBindingsRevision,
                                              body.to<JsonObject>());
                 sendJson(out, 200, body);
             });
}

}  // namespace hubbo::api
