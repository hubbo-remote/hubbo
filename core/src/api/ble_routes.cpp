// POST/GET/DELETE /admin/devices/{id}/pair and DELETE /admin/devices/{id}/pairing:
// pairing one Bluetooth device's host, over the platform's BleHostPort
// (integrations/ble_hosts.h). Also each Bluetooth device's `bluetooth` status.

#include "routes.h"
#include "support.h"

namespace hubbo::api {

using integrations::BleHostResult;
using integrations::BleHostStatus;

namespace {

void sendBleUnavailable(http::Response &out) { sendError(out, 503, "integration_unavailable", "Bluetooth isn't working"); }

// The Bluetooth device `id` with a working radio, or false after a 404/503.
bool requireBleDevice(const ApiContext &ctx, std::string_view id, http::Response &out) {
    const DeviceSpec *device = requireDevice(ctx, id, out);
    if (device == nullptr) return false;
    if (device->provider.type != kBleHidType) {
        sendError(out, 404, "not_found", device->name + " isn't a Bluetooth device");
        return false;
    }
    if (ctx.bleHosts == nullptr || !ctx.bleHosts->radioReady()) {
        sendBleUnavailable(out);
        return false;
    }
    return true;
}

void sendStatus(const BleHostStatus &status, http::Response &out, int code) {
    JsonDocument doc;
    putBluetoothStatus(doc.to<JsonObject>(), status);
    sendJson(out, code, doc);
}

// ?window_s=30..300 (seconds), default kBlePairingDefaultWindowMs.
bool readWindow(const http::Request &request, http::Response &out, uint32_t *windowMs) {
    using namespace integrations;
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

// Accepted, or the 409/503 for a refused request (false).
bool accepted(BleHostResult result, http::Response &out) {
    switch (result) {
        case BleHostResult::Accepted: return true;
        case BleHostResult::Busy: sendError(out, 409, "busy", "Another device is pairing"); return false;
        case BleHostResult::Full: sendError(out, 503, "busy", "Bluetooth is busy. Try again."); return false;
        case BleHostResult::NotReady: sendBleUnavailable(out); return false;
    }
    return false;
}

}  // namespace

void putBluetoothStatus(JsonObject out, const BleHostStatus &status) {
    const integrations::BleDeviceState s = integrations::bleDeviceState(status);
    out["state"] = s.state;
    if (std::string_view(s.state) == "pairing") out["window_s"] = s.windowS;
}

void addBleRoutes(RouteSink &sink, ApiContext &ctx) {
    sink.add("/api/v1/admin/devices/{device_id}/pair", auth::Operation::StartDevicePairing,
             [&ctx](auto &request, auto &p, auto &out) {
                 uint32_t windowMs = 0;
                 if (!requireBleDevice(ctx, p[0], out) || !readWindow(request, out, &windowMs)) return;
                 if (!accepted(ctx.bleHosts->startPairing(p[0], windowMs), out)) return;
                 BleHostStatus status = ctx.bleHosts->status(p[0]);
                 if (!status.pairing) {  // the BLE loop hasn't opened it yet
                     status.pairing = true;
                     status.windowRemainingMs = windowMs;
                 }
                 sendStatus(status, out, 202);
             });

    sink.add("/api/v1/admin/devices/{device_id}/pair", auth::Operation::GetDevicePairing,
             [&ctx](auto &, auto &p, auto &out) {
                 if (!requireBleDevice(ctx, p[0], out)) return;
                 sendStatus(ctx.bleHosts->status(p[0]), out, 200);
             });

    sink.add("/api/v1/admin/devices/{device_id}/pair", auth::Operation::CancelDevicePairing,
             [&ctx](auto &, auto &p, auto &out) {
                 if (!requireBleDevice(ctx, p[0], out) || !accepted(ctx.bleHosts->cancelPairing(p[0]), out)) return;
                 sendEmpty(out, 204);
             });

    sink.add("/api/v1/admin/devices/{device_id}/pairing", auth::Operation::ForgetDevicePairing,
             [&ctx](auto &, auto &p, auto &out) {
                 if (!requireBleDevice(ctx, p[0], out) || !accepted(ctx.bleHosts->forget(p[0]), out)) return;
                 sendEmpty(out, 204);
             });
}

}  // namespace hubbo::api
