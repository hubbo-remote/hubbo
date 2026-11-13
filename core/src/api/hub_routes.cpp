// GET/PUT /admin/hub (the hub's name, over config::HubSettings) and
// POST /admin/restart (over RestartPort).

#include "hubbo/api/transitions.h"
#include "hubbo/config/hub.h"
#include "routes.h"
#include "support.h"

namespace hubbo::api {

namespace {

void sendHub(http::Response &out, const config::HubIdentity &identity) {
    JsonDocument doc;
    doc["name"] = identity.name;
    doc["hostname"] = identity.shownHostname();
    doc["hostname_conflict"] = identity.hostnameConflict();
    sendJson(out, 200, doc);
}

void sendNameIssue(http::Response &out, ErrorCode code) {
    const std::string message = code == ErrorCode::Required ? std::string("Enter a name")
                                : code == ErrorCode::TooLong
                                    ? "Name must be at most " + std::to_string(config::kHubNameMaxChars) + " characters"
                                    : std::string("Name must be text without control characters");
    sendDetail(out, "/name", errorCodeName(code), message);
}

}  // namespace

void addHubRoutes(RouteSink &sink, ApiContext &ctx) {
    sink.add("/api/v1/admin/hub", auth::Operation::GetHub, [&ctx](auto &, auto &, auto &out) {
        if (ctx.hub == nullptr) return sendError(out, 503, "unavailable", "This hub can't be named");
        sendHub(out, ctx.hub->identity());
    });

    sink.add("/api/v1/admin/hub", auth::Operation::UpdateHub, [&ctx](auto &request, auto &, auto &out) {
        if (ctx.hub == nullptr) return sendError(out, 503, "unavailable", "This hub can't be named");
        JsonDocument doc;
        if (!readJson(request, out, doc)) return;
        ValidationReport report;
        if (!checkObject(doc.as<JsonVariantConst>(), {{"name", Field::Kind::String, true}}, &report)) {
            return sendIssues(out, report, "The name is invalid");
        }
        config::HubIdentity identity;
        const ErrorCode result = ctx.hub->rename(doc["name"].as<std::string_view>(), &identity);
        if (result == ErrorCode::StorageFailed) return sendCoreError(out, result, "hub name");
        if (result != ErrorCode::Ok) return sendNameIssue(out, result);
        sendHub(out, identity);
    });

    sink.add("/api/v1/admin/restart", auth::Operation::RestartHub, [&ctx](auto &, auto &, auto &out) {
        switch (requestHubRestart(ctx.restart, ctx.firmware)) {
            case RestartOutcome::Accepted: break;
            case RestartOutcome::NoPort: return sendError(out, 503, "unavailable", "This hub can't restart");
            case RestartOutcome::UpdateInProgress:
                return sendError(out, 409, "update_in_progress", "A firmware update is running");
            case RestartOutcome::Unavailable:
                return sendError(out, 503, "unavailable", "The hub can't restart right now");
        }
        JsonDocument doc;
        doc.to<JsonObject>();
        sendJson(out, 202, doc);
    });
}

}  // namespace hubbo::api
