// GET /admin/remote, PUT /admin/remote/endpoint and POST/GET/DELETE
// /admin/remote/pair, over the platform's RemotePort (remote_port.h).

#include "hubbo/api/remote_port.h"
#include "routes.h"
#include "support.h"

namespace hubbo::api {

namespace {

void putPairing(JsonObject out, const RemoteRadioStatus &s) {
    out["state"] = scanStateName(s.scan);
    out["paired"] = s.hasEndpoint;
    if (s.scan == ScanState::Scanning) {
        out["remaining_s"] = (s.scanRemainingMs + 999) / 1000;
        out["channel"] = s.scanChannel;
    } else if (s.hasEndpoint) {
        out["channel"] = s.endpoint.channel;
    }
}

void putRemote(JsonObject out, const RemoteRadioStatus &s) {
    out["radio"] = radioStateName(s.radio);
    if (s.hasEndpoint) {
        JsonObject endpoint = out["endpoint"].to<JsonObject>();
        endpoint["channel"] = s.endpoint.channel;
        endpoint["address"] = formatRemoteAddress(s.endpoint.address);
    } else {
        out["endpoint"] = nullptr;
    }
    putPairing(out["pairing"].to<JsonObject>(), s);
}

bool requireRemote(const ApiContext &ctx, http::Response &out) {
    if (ctx.remote != nullptr) return true;
    sendError(out, 503, "integration_unavailable", "This hub has no remote radio");
    return false;
}

void sendRemoteResult(http::Response &out, RemoteResult result) {
    switch (result) {
        case RemoteResult::Accepted: return;
        case RemoteResult::Invalid:
            return sendDetail(out, "/address", "pattern",
                              "Not a usable endpoint: channel 0-124, a 40-bit address that isn't all zeros or "
                              "ones, low byte not 55 or AA");
        case RemoteResult::NotReady: return sendError(out, 503, "integration_unavailable", "The remote radio isn't working");
        case RemoteResult::Busy: return sendError(out, 409, "busy", "A Hub scan is running; cancel it first");
    }
}

}  // namespace

void addRemoteRoutes(RouteSink &sink, ApiContext &ctx) {
    sink.add("/api/v1/admin/remote", auth::Operation::GetRemoteConfig, [&ctx](auto &, auto &, auto &out) {
        if (!requireRemote(ctx, out)) return;
        JsonDocument doc;
        putRemote(doc.to<JsonObject>(), ctx.remote->status());
        sendJson(out, 200, doc);
    });

    sink.add("/api/v1/admin/remote/endpoint", auth::Operation::SetRemoteEndpoint,
             [&ctx](auto &request, auto &, auto &out) {
                 if (!requireRemote(ctx, out)) return;
                 JsonDocument doc;
                 if (!readJson(request, out, doc)) return;
                 ValidationReport report;
                 if (!checkObject(doc.as<JsonVariantConst>(),
                                  {{"channel", Field::Kind::Integer, true}, {"address", Field::Kind::String, true, 12}},
                                  &report)) {
                     return sendIssues(out, report, "The endpoint is invalid");
                 }
                 const int64_t channel = doc["channel"].as<int64_t>();
                 if (channel < 0 || channel > kMaxRemoteChannel) {
                     return sendDetail(out, "/channel", "param_out_of_range", "channel must be 0-124");
                 }
                 RemoteEndpoint endpoint;
                 endpoint.channel = static_cast<uint8_t>(channel);
                 if (!parseRemoteAddress(doc["address"].as<const char *>(), &endpoint.address)) {
                     return sendDetail(out, "/address", "pattern", "address must be up to 10 hex digits (0x optional)");
                 }
                 const RemoteResult result = ctx.remote->setEndpoint(endpoint);
                 if (result != RemoteResult::Accepted) return sendRemoteResult(out, result);
                 // The radio applies it on its own loop; report what was set.
                 RemoteRadioStatus status = ctx.remote->status();
                 status.hasEndpoint = true;
                 status.endpoint = endpoint;
                 JsonDocument body;
                 putRemote(body.to<JsonObject>(), status);
                 sendJson(out, 200, body);
             });

    sink.add("/api/v1/admin/remote/pair", auth::Operation::GetRemotePairing, [&ctx](auto &, auto &, auto &out) {
        if (!requireRemote(ctx, out)) return;
        JsonDocument doc;
        putPairing(doc.to<JsonObject>(), ctx.remote->status());
        sendJson(out, 200, doc);
    });

    sink.add("/api/v1/admin/remote/pair", auth::Operation::StartRemotePairing, [&ctx](auto &, auto &, auto &out) {
        if (!requireRemote(ctx, out)) return;
        const RemoteResult result = ctx.remote->startScan(kRemoteScanTimeoutMs);
        if (result != RemoteResult::Accepted) return sendRemoteResult(out, result);
        RemoteRadioStatus status = ctx.remote->status();
        if (status.scan != ScanState::Scanning) {  // the loop hasn't picked it up yet
            status.scan = ScanState::Scanning;
            status.scanRemainingMs = kRemoteScanTimeoutMs;
        }
        JsonDocument doc;
        putPairing(doc.to<JsonObject>(), status);
        sendJson(out, 202, doc);
    });

    sink.add("/api/v1/admin/remote/pair", auth::Operation::CancelRemotePairing, [&ctx](auto &, auto &, auto &out) {
        if (!requireRemote(ctx, out)) return;
        ctx.remote->stopScan();
        sendEmpty(out, 204);
    });
}

}  // namespace hubbo::api
