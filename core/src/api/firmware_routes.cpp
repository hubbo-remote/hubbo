// GET /admin/firmware and POST /admin/firmware/update, over the platform's
// FirmwareUpdatePort (firmware_update_port.h). The transport has buffered
// the whole image; nothing is erased until it has passed every check.
// POST /admin/firmware/check and /install, over FirmwareReleasePort
// (firmware_release_port.h): signed releases, downloaded by the hub.
// POST /admin/factory-reset, over FactoryResetPort: the other route that
// ends in a restart.

#include "hubbo/api/firmware_release_port.h"
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
    putNullableId(update, "error", s.error);
    // Releases: never checked when the platform has no feed.
    const FirmwareReleaseStatus r = ctx.releases != nullptr ? ctx.releases->releaseStatus() : FirmwareReleaseStatus{};
    const release::UpdateCheckStatus &check = r.check;
    if (check.available) {
        JsonObject available = doc["available"].to<JsonObject>();
        available["version"] = check.available->version;
    } else {
        doc["available"] = nullptr;
    }
    doc["checking"] = check.checking;
    if (check.checked) {
        doc["checked_ago_ms"] = r.checkedAgoMs;
    } else {
        doc["checked_ago_ms"] = nullptr;
    }
    const char *error = release::checkErrorName(check.error);
    putNullableId(doc.as<JsonObject>(), "check_error", error != nullptr ? error : "");
    putNullableId(doc.as<JsonObject>(), "rolled_back_from", check.rolledBackFrom);
    sendJson(out, status, doc);
}

void sendNoReleases(http::Response &out) { sendError(out, 503, "unavailable", "This hub can't check for releases"); }

// The 4xx/5xx for a refused install (Accepted sends the status instead).
void sendInstallError(http::Response &out, FirmwareInstallResult result) {
    using R = FirmwareInstallResult;
    const char *invalid = nullptr;
    switch (result) {
        case R::Accepted: return;
        case R::Busy: return sendError(out, 409, "update_in_progress", "An update is already running");
        case R::Unavailable: return sendError(out, 503, "unavailable", "The hub can't take an update right now");
        case R::TooLarge: return sendError(out, 413, "payload_too_large", "The image is larger than the update slot");
        case R::WrongBoard: return sendError(out, 422, "wrong_board", "This update is for a different device");
        case R::NotAnImage:  // e.g. the merged factory image instead of firmware.bin
        case R::ForeignImage: invalid = "This isn't a Hubbo update file"; break;
        case R::WrongChip: invalid = "This update is for a different device"; break;
        case R::WrongLayout: invalid = "This update has to be installed over USB"; break;
        case R::Corrupt: invalid = "This update file is damaged; download it again"; break;
    }
    sendError(out, 422, "invalid_image", invalid);
}

}  // namespace

void addFirmwareRoutes(RouteSink &sink, ApiContext &ctx) {
    sink.add("/api/v1/admin/firmware", auth::Operation::GetFirmwareStatus, [&ctx](auto &, auto &, auto &out) {
        if (ctx.firmware == nullptr) return sendNoUpdates(out);
        sendFirmwareStatus(ctx, out, 200);
    });

    sink.add("/api/v1/admin/firmware/check", auth::Operation::CheckFirmwareRelease,
             [&ctx](auto &, auto &, auto &out) {
                 if (ctx.firmware == nullptr || ctx.releases == nullptr) return sendNoReleases(out);
                 if (ctx.releases->requestCheck() == ReleaseCheckResult::Busy) {
                     return sendError(out, 409, "busy", "A check is already running");
                 }
                 sendFirmwareStatus(ctx, out, 202);
             });

    sink.add("/api/v1/admin/firmware/install", auth::Operation::InstallFirmwareRelease,
             [&ctx](auto &request, auto &, auto &out) {
                 if (ctx.firmware == nullptr || ctx.releases == nullptr) return sendNoReleases(out);
                 JsonDocument doc;
                 if (!readJson(request, out, doc)) return;
                 ValidationReport report;
                 if (!checkObject(doc.as<JsonVariantConst>(), {{"version", Field::Kind::String, true, 32}}, &report)) {
                     return sendIssues(out, report, "Name the version to install");
                 }
                 const std::string_view version = doc["version"].as<std::string_view>();
                 release::Version parsed;
                 if (!release::parseVersion(version, &parsed)) {
                     return sendDetail(out, "/version", "pattern", "A version is X.Y.Z");
                 }
                 switch (ctx.releases->install(version)) {
                     case ReleaseInstallResult::Accepted: break;
                     case ReleaseInstallResult::Busy:
                         return sendInstallError(out, FirmwareInstallResult::Busy);
                     case ReleaseInstallResult::NotAvailable:
                         return sendError(out, 409, "not_available", "That version isn't available; check again");
                 }
                 sendFirmwareStatus(ctx, out, 202);
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
