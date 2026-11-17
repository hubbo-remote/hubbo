#include "release_feed.h"

#include <ArduinoJson.h>

#include <cstring>
#include <vector>

#include "hubbo/api/firmware_update_port.h"
#include "hubbo/auth/crypto.h"

namespace hubbo::sim {

std::string simulatorImage(const std::string &version, const std::string &board, uint16_t layout) {
    std::string b(32, '\0');
    b[0] = '\xE9';
    b[12] = static_cast<char>(api::kEsp32S3ChipId);
    b[23] = 1;  // hash appended
    std::string desc(256, '\0');
    desc[0] = '\x32';
    desc[1] = '\x54';
    desc[2] = '\xCD';
    desc[3] = '\xAB';  // app desc magic 0xABCD5432
    b += desc;
    b += "HUBBO-IMAGE/1 board=" + board + " layout=" + std::to_string(layout) + " version=" + version;
    b += '\0';
    b.resize(512, '\xFF');
    uint8_t digest[auth::kSha256Size];
    auth::SoftwareHash().sha256(reinterpret_cast<const uint8_t *>(b.data()), b.size(), digest);
    b.append(reinterpret_cast<const char *>(digest), sizeof(digest));
    return b;
}

std::string FakeReleaseFeed::manifestJson() const {
    const std::string bin = image();
    auth::SoftwareHash hash;
    JsonDocument doc;
    doc["version"] = version;
    doc["board"] = board;
    doc["layout"] = layout;
    doc["size"] = bin.size();
    doc["sha256"] = release::sha256Hex(bin, hash);
    if (!minVersion.empty()) doc["min_version"] = minVersion;
    std::string out;
    serializeJson(doc, out);
    return out;
}

release::FetchResult FakeReleaseFeed::fetchManifest(std::string *manifest) {
    if (!published || fault == release::CheckError::Unreachable) return release::FetchResult::Unreachable;
    if (fault == release::CheckError::BadSignature) return release::FetchResult::BadSignature;
    *manifest = fault == release::CheckError::BadManifest ? R"({"version":"one"})" : manifestJson();
    return release::FetchResult::Ok;
}

release::FetchResult FakeReleaseFeed::fetchImage(const release::Manifest &manifest, uint8_t *buffer, size_t capacity,
                                                 size_t *length) {
    // The release's image, whatever this manifest says (a feed that moved
    // on serves the new one: the size and sha256 checks catch it).
    if (!published || fault == release::CheckError::Unreachable || manifest.version.empty()) {
        return release::FetchResult::Unreachable;
    }
    const std::string bin = image();
    if (bin.size() > capacity) return release::FetchResult::Unreachable;
    std::memcpy(buffer, bin.data(), bin.size());
    *length = bin.size();
    return release::FetchResult::Ok;
}

SimFirmwareReleases::SimFirmwareReleases(api::SimulatedFirmwareUpdate &firmware, std::function<uint32_t()> nowMs)
    : firmware_(firmware), nowMs_(std::move(nowMs)) {}

void SimFirmwareReleases::setVersion(std::string version) {
    version_ = std::move(version);
    if (onVersion) onVersion(version_);
}

release::RunningFirmware SimFirmwareReleases::running() const {
    const api::FirmwareTarget target = api::simulatorFirmwareTarget();
    release::RunningFirmware r;
    r.version = version_;
    r.board = target.board;
    r.layout = target.layout;
    r.slotBytes = target.slotBytes;
    return r;
}

api::FirmwareReleaseStatus SimFirmwareReleases::releaseStatus() const {
    api::FirmwareReleaseStatus s;
    s.check = state_.read();
    if (s.check.checked) s.checkedAgoMs = nowMs_() - s.check.checkedAtMs;
    return s;
}

api::ReleaseCheckResult SimFirmwareReleases::requestCheck() {
    if (!state_.beginCheck()) return api::ReleaseCheckResult::Busy;
    checkLeftMs_ = kCheckMs;
    return api::ReleaseCheckResult::Accepted;
}

api::ReleaseInstallResult SimFirmwareReleases::install(std::string_view version) {
    if (api::firmwareUpdateRunning(firmware_.status().state)) return api::ReleaseInstallResult::Busy;
    std::optional<release::Manifest> manifest = state_.availableAs(version);
    if (!manifest) return api::ReleaseInstallResult::NotAvailable;
    if (!firmware_.beginDownload()) return api::ReleaseInstallResult::Busy;
    state_.clearRolledBackFrom();
    downloading_ = std::move(manifest);
    downloadLeftMs_ = kDownloadMs;
    return api::ReleaseInstallResult::Accepted;
}

uint32_t SimFirmwareReleases::advance(uint32_t elapsedMs) {
    if (checkLeftMs_) {
        if (*checkLeftMs_ > elapsedMs) {
            *checkLeftMs_ -= elapsedMs;
        } else {
            checkLeftMs_.reset();
            state_.finishCheck(release::checkForUpdate(feed, running()), nowMs_());
        }
    }
    if (!downloading_) return elapsedMs;
    if (downloadLeftMs_ > elapsedMs) {
        downloadLeftMs_ -= elapsedMs;
        return 0;
    }
    const uint32_t rest = elapsedMs - downloadLeftMs_;
    download();
    return rest;
}

void SimFirmwareReleases::download() {
    const release::Manifest manifest = std::move(*downloading_);
    downloading_.reset();
    std::vector<uint8_t> buffer(manifest.size);
    size_t length = 0;
    auth::SoftwareHash hash;
    switch (release::downloadImage(feed, manifest, buffer.data(), buffer.size(), hash, &length)) {
        case release::DownloadResult::Failed: firmware_.failDownload(api::kDownloadFailed); return;
        case release::DownloadResult::Mismatch: firmware_.failDownload(api::kImageMismatch); return;
        case release::DownloadResult::Ok: break;
    }
    // Validated as an upload; invalid_image on failure.
    (void)firmware_.finishDownload(std::string_view(reinterpret_cast<const char *>(buffer.data()), length));
}

void SimFirmwareReleases::rebooted() {
    state_.forgetChecks();
    checkLeftMs_.reset();
    setVersion(firmware_.lastVersion());
}

void SimFirmwareReleases::reset() {
    feed.reset();
    state_.forgetChecks();
    state_.clearRolledBackFrom();
    checkLeftMs_.reset();
    downloading_.reset();
    setVersion(kDefaultVersion);
}

}  // namespace hubbo::sim
