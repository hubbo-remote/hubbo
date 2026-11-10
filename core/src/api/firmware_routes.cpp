// GET /admin/firmware and POST /admin/firmware/update, over the platform's
// FirmwareUpdatePort (firmware_update_port.h). The transport has buffered
// the whole image; nothing is erased until it has passed every check.
// POST /admin/factory-reset, over FactoryResetPort: the other route that
// ends in a restart.

#include "hubbo/api/firmware_update_port.h"
#include "routes.h"
#include "support.h"

namespace hubbo::api {

namespace {

void sendNoUpdates(http::Response &out) { sendError(out, 503, "unavailable", "This hub can't take a firmware update"); }

void sendFirmwareStatus(const ApiContext &ctx, http::Response &out, int status) {
    const FirmwareUpdateStatus s = ctx.firmware->status();
    JsonDocument doc;
    doc["version"] = ctx.info.version;
    doc["board"] = ctx.info.board;
    JsonObject update = doc["update"].to<JsonObject>();
    update["state"] = firmwareUpdateStateName(s.state);
    update["progress"] = s.progress;
    if (s.error.empty()) {
        update["error"] = nullptr;
    } else {
        update["error"] = s.error;
    }
    sendJson(out, status, doc);
}

// The 4xx/5xx for a refused install (Accepted sends the status instead).
void sendInstallError(http::Response &out, FirmwareInstallResult result) {
    using R = FirmwareInstallResult;
    const char *invalid = nullptr;
    switch (result) {
        case R::Accepted: return;
        case R::Busy: return sendError(out, 409, "update_in_progress", "An update is already running");
        case R::Unavailable: return sendError(out, 503, "unavailable", "The hub can't take an update right now");
        case R::TooLarge: return sendError(out, 413, "payload_too_large", "The image is larger than the update slot");
        case R::WrongBoard: return sendError(out, 422, "wrong_board", "This Hubbo image is for another board");
        case R::NotAnImage:
            invalid = "Not an app image (use firmware.bin, not the merged factory image)";
            break;
        case R::WrongChip: invalid = "This image is for another chip or a newer chip revision"; break;
        case R::ForeignImage: invalid = "Not a Hubbo firmware image"; break;
        case R::WrongLayout: invalid = "This image needs another partition layout; flash it over USB"; break;
        case R::Corrupt: invalid = "The image's checksum doesn't match; download it again"; break;
    }
    sendError(out, 422, "invalid_image", invalid);
}

}  // namespace

void addFirmwareRoutes(RouteSink &sink, ApiContext &ctx) {
    sink.add("/api/v1/admin/firmware", auth::Operation::GetFirmwareStatus, [&ctx](auto &, auto &, auto &out) {
        if (ctx.firmware == nullptr) return sendNoUpdates(out);
        sendFirmwareStatus(ctx, out, 200);
    });

    // The router has checked the size (the policy table's OTA slot limit)
    // and the octet-stream type of a non-empty body.
    sink.add("/api/v1/admin/firmware/update", auth::Operation::UpdateFirmware,
             [&ctx](auto &request, auto &, auto &out) {
                 if (ctx.firmware == nullptr) return sendNoUpdates(out);
                 if (request.headers == nullptr || request.headers->header("Content-Length").empty()) {
                     return sendError(out, 411, "length_required", "Send the image whole, with Content-Length");
                 }
                 const FirmwareInstallResult result = ctx.firmware->install(request.body);
                 if (result != FirmwareInstallResult::Accepted) return sendInstallError(out, result);
                 sendFirmwareStatus(ctx, out, 202);
             });

    sink.add("/api/v1/admin/factory-reset", auth::Operation::FactoryReset, [&ctx](auto &, auto &, auto &out) {
        if (ctx.factoryReset == nullptr) {
            return sendError(out, 503, "unavailable", "This hub can't do a factory reset");
        }
        if (ctx.firmware != nullptr && firmwareUpdateRunning(ctx.firmware->status().state)) {
            return sendError(out, 409, "update_in_progress", "A firmware update is running");
        }
        if (!ctx.factoryReset->requestFactoryReset()) {
            return sendError(out, 503, "unavailable", "The hub can't do a factory reset right now");
        }
        sendEmpty(out, 202);
    });
}

}  // namespace hubbo::api
