// POST/GET/DELETE /admin/integrations/{id}/pair: BLE HID pairing for the
// ble_hid integration, over the platform's BlePairingPort (ble_pairing_port.h).

#include "hubbo/api/ble_pairing_port.h"
#include "hubbo/config/service.h"
#include "routes.h"
#include "support.h"

namespace hubbo::api {

namespace {

void sendBleUnavailable(http::Response &out) {
    sendError(out, 503, "integration_unavailable", "Bluetooth isn't running");
}

// The ble_hid integration's pairing port, or false after a 404/503.
bool requireBlePairing(const ApiContext &ctx, std::string_view id, http::Response &out) {
    const IntegrationSpec *integration = ctx.config.findIntegration(id);
    if (integration == nullptr || integration->type != kBleHidType) {
        sendError(out, 404, "not_found", "No such Bluetooth integration");
        return false;
    }
    if (ctx.blePairing == nullptr || ctx.blePairing->status().state == BlePairState::NotReady) {
        sendBleUnavailable(out);
        return false;
    }
    return true;
}

void sendPairing(const BlePairingStatus &s, http::Response &out, int status) {
    JsonDocument doc;
    doc["state"] = blePairStateName(s.state);
    if (s.state == BlePairState::Pairing) doc["window_s"] = (s.windowRemainingMs + 999) / 1000;
    if (s.state == BlePairState::Paired && !s.peerName.empty()) doc["peer_name"] = s.peerName;
    sendJson(out, status, doc);
}

// ?window_s=30..300 (seconds), default kBlePairingDefaultWindowMs.
bool readWindow(const http::Request &request, http::Response &out, uint32_t *windowMs) {
    *windowMs = kBlePairingDefaultWindowMs;
    const std::string text = http::queryValue(request.query, "window_s");
    if (text.empty()) return true;
    uint64_t seconds = 0;
    if (!parseDecimal(text, kBlePairingMaxWindowMs / 1000, &seconds) || seconds * 1000 < kBlePairingMinWindowMs) {
        sendDetail(out, "/window_s", "param_out_of_range", "window_s must be 30-300");
        return false;
    }
    *windowMs = static_cast<uint32_t>(seconds * 1000);
    return true;
}

}  // namespace

void addBleRoutes(RouteSink &sink, ApiContext &ctx) {
    sink.add("/api/v1/admin/integrations/{integration_id}/pair", auth::Operation::StartIntegrationPairing,
             [&ctx](auto &request, auto &p, auto &out) {
                 uint32_t windowMs = 0;
                 if (!requireBlePairing(ctx, p[0], out) || !readWindow(request, out, &windowMs)) return;
                 switch (ctx.blePairing->startPairing(windowMs)) {
                     case BlePairingResult::Accepted: break;
                     case BlePairingResult::Busy:
                         return sendError(out, 409, "busy", "A pairing window is already open");
                     case BlePairingResult::NotReady: return sendBleUnavailable(out);
                 }
                 BlePairingStatus status = ctx.blePairing->status();
                 if (status.state != BlePairState::Pairing) {  // the BLE loop hasn't opened it yet
                     status = BlePairingStatus{BlePairState::Pairing, windowMs, ""};
                 }
                 sendPairing(status, out, 202);
             });

    sink.add("/api/v1/admin/integrations/{integration_id}/pair", auth::Operation::GetIntegrationPairing,
             [&ctx](auto &, auto &p, auto &out) {
                 if (!requireBlePairing(ctx, p[0], out)) return;
                 sendPairing(ctx.blePairing->status(), out, 200);
             });

    sink.add("/api/v1/admin/integrations/{integration_id}/pair", auth::Operation::ForgetIntegrationPairing,
             [&ctx](auto &, auto &p, auto &out) {
                 if (!requireBlePairing(ctx, p[0], out)) return;
                 if (ctx.blePairing->forget() != BlePairingResult::Accepted) return sendBleUnavailable(out);
                 sendEmpty(out, 204);
             });
}

}  // namespace hubbo::api
