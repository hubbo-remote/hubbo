#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "hubbo/api/firmware_release_port.h"
#include "hubbo/integrations/firmware_release.h"

// hub-sim's release feed (docs/design/firmware-updates.md): a fake "latest
// release" the contract and e2e drive through POST /sim/release, so a check
// and an install run end to end without GitHub. No signatures: `fault`
// stands in for a bad one.
namespace hubbo::sim {

// The smallest app image SimulatedFirmwareUpdate accepts: image header, app
// desc, the Hubbo marker for `board`/`layout`/`version`, padding and the
// appended SHA-256 (core/tests/firmware_image_util.h has the full builder).
std::string simulatorImage(const std::string &version, const std::string &board = "simulator", uint16_t layout = 1);

class FakeReleaseFeed : public release::ReleaseFeed {
public:
    release::FetchResult fetchManifest(std::string *manifest) override;
    release::FetchResult fetchImage(const release::Manifest &manifest, uint8_t *buffer, size_t capacity,
                                    size_t *length) override;

    // The latest release. `published` false: there is none (a check is
    // unreachable, as GitHub's 404 is).
    bool published = false;
    std::string version = "1.0.0";
    std::string board = "simulator";
    uint16_t layout = 1;
    std::string minVersion;  // "" absent
    // The check error a feed fault causes (BadManifest: signed, but not a
    // manifest this hub reads); None: a good feed.
    release::CheckError fault = release::CheckError::None;

    // Its image, as the manifest describes it.
    std::string image() const { return simulatorImage(version, board, layout); }
    std::string manifestJson() const;
    void reset() { *this = FakeReleaseFeed(); }
};

// hub-sim's FirmwareReleasePort, over the fake feed and the sim's one
// SimulatedFirmwareUpdate (so an install's download, write and reboot are one
// update state, as on the board). A check takes kCheckMs and a download
// kDownloadMs of hub time (advance()), so "checking" and "downloading" show.
// No scheduled checks: only Check now. Single task.
//
// It owns the version the hub runs: what a check compares against, and,
// through onVersion (SimApi: Api::setFirmwareVersion), HubInfo::version.
class SimFirmwareReleases : public api::FirmwareReleasePort {
public:
    static constexpr uint32_t kCheckMs = 500;
    static constexpr uint32_t kDownloadMs = 1000;
    static constexpr const char *kDefaultVersion = "0.0.0-sim";  // a development build: never offered one

    SimFirmwareReleases(api::SimulatedFirmwareUpdate &firmware, std::function<uint32_t()> nowMs);

    api::FirmwareReleaseStatus releaseStatus() const override;
    api::ReleaseCheckResult requestCheck() override;
    api::ReleaseInstallResult install(std::string_view version) override;

    // Runs a check or download that's due; returns how much of `elapsedMs`
    // the update itself (SimulatedFirmwareUpdate::advance) gets: none while
    // downloading, the rest once the download hands off the image.
    uint32_t advance(uint32_t elapsedMs);
    // The simulated reboot into a new image: it runs that image's version,
    // and, as the board's RAM, forgets every check.
    void rebooted();
    // /sim/reset: no release, nothing checked or rolled back, the default version.
    void reset();

    const std::string &version() const { return version_; }
    void setVersion(std::string version);
    std::function<void(const std::string &version)> onVersion;

    // Simulation controls (/sim/release).
    FakeReleaseFeed feed;
    release::UpdateCheckState &state() { return state_; }

private:
    release::RunningFirmware running() const;
    void download();

    api::SimulatedFirmwareUpdate &firmware_;
    std::function<uint32_t()> nowMs_;
    std::string version_ = kDefaultVersion;
    release::UpdateCheckState state_;
    std::optional<uint32_t> checkLeftMs_;
    std::optional<release::Manifest> downloading_;
    uint32_t downloadLeftMs_ = 0;
};

}  // namespace hubbo::sim
