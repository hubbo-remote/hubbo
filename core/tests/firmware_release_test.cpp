// Online firmware updates, the portable side (docs/design/firmware-updates.md):
// semver, the manifest, availability, the image check, one check against a
// feed, and the state the API reads.
#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "hub.h"
#include "hubbo/integrations/firmware_release.h"
#include "release_feed.h"

using namespace hubbo;
using namespace hubbo::release;

namespace {

constexpr size_t kSlot = 64 * 1024;
const std::string kSha(64, 'a');

Version v(const char *text) {
    Version out;
    EXPECT_TRUE(parseVersion(text, &out)) << text;
    return out;
}

// A valid manifest with one field replaced (or added): `field` is "name":value.
std::string with(const std::string &name, const std::string &value) {
    std::string base = R"({"version":"1.4.0","board":"simulator","layout":1,"size":1000,"sha256":")" + kSha +
                       R"(","notes":"Faster."})";
    const std::string key = "\"" + name + "\":";
    const size_t at = base.find(key);
    if (at == std::string::npos) return base.substr(0, base.size() - 1) + "," + key + value + "}";
    size_t end = at + key.size();
    if (base[end] == '"') {
        end = base.find('"', end + 1) + 1;
    } else {
        while (base[end] != ',' && base[end] != '}') end++;
    }
    return base.substr(0, at) + key + value + base.substr(end);
}

RunningFirmware running(const char *version = "1.3.2") {
    RunningFirmware r;
    r.version = version;
    r.board = "simulator";
    r.layout = 1;
    r.slotBytes = kSlot;
    return r;
}

Manifest offered(const char *version, const char *minVersion = "") {
    Manifest m;
    m.version = version;
    m.board = "simulator";
    m.layout = 1;
    m.size = 1000;
    m.sha256 = kSha;
    m.minVersion = minVersion;
    return m;
}

}  // namespace

TEST(FirmwareVersion, OnlyAPlainXYZIsARelease) {
    EXPECT_EQ(v("1.4.0").major, 1);
    EXPECT_EQ(v("0.0.0").patch, 0);
    EXPECT_EQ(v("65535.10.200").minor, 10);
    Version out;
    for (const char *dev : {"", "unknown", "v1.4.0", "1.4", "1.4.0.1", "1.4.0-rc1", "1.4.0-3-gabc1234",
                            "1.4.0-dirty", "v1.4.0-3-gabc1234-dirty", "01.4.0", "1.04.0", "1..0", "1.4.0 ",
                            " 1.4.0", "+1.4.0", "-1.4.0", "65536.0.0", "1.4.x"}) {
        EXPECT_FALSE(parseVersion(dev, &out)) << dev;
    }
}

TEST(FirmwareVersion, ComparesBySemver) {
    EXPECT_LT(compareVersions(v("1.4.0"), v("1.10.0")), 0);  // numeric, not text
    EXPECT_LT(compareVersions(v("1.4.9"), v("1.5.0")), 0);
    EXPECT_LT(compareVersions(v("1.9.9"), v("2.0.0")), 0);
    EXPECT_GT(compareVersions(v("1.4.1"), v("1.4.0")), 0);
    EXPECT_EQ(compareVersions(v("1.4.0"), v("1.4.0")), 0);
}

TEST(FirmwareManifest, ParsesEveryField) {
    Manifest m;
    std::string field;
    ASSERT_TRUE(parseManifest(with("min_version", "\"1.2.0\""), kSlot, &m, &field)) << field;
    EXPECT_EQ(m.version, "1.4.0");
    EXPECT_EQ(m.board, "simulator");
    EXPECT_EQ(m.layout, 1);
    EXPECT_EQ(m.size, 1000u);
    EXPECT_EQ(m.sha256, kSha);
    EXPECT_EQ(m.minVersion, "1.2.0");
    // min_version is optional; an unknown field is ignored.
    ASSERT_TRUE(parseManifest(with("channel", "\"stable\""), kSlot, &m, &field)) << field;
    EXPECT_TRUE(m.minVersion.empty());
    // CI's "notes" (trimmed to 2 KB) is one of those, whatever it holds.
    EXPECT_TRUE(parseManifest(with("notes", "\"" + std::string(2048, 'n') + "\""), kSlot, &m));
    EXPECT_TRUE(parseManifest(with("notes", "null"), kSlot, &m));
    // The size up to the slot.
    EXPECT_TRUE(parseManifest(with("size", std::to_string(kSlot)), kSlot, &m));
}

TEST(FirmwareManifest, IsStrictAboutTypesAndLimits) {
    const auto refused = [](const std::string &json, const char *expected) {
        Manifest m;
        std::string field = "unset";
        EXPECT_FALSE(parseManifest(json, kSlot, &m, &field)) << json;
        EXPECT_EQ(field, expected) << json;
    };
    refused("", "");
    refused("[]", "");
    refused("not json", "");
    refused(R"({"version":"1.4.0")", "");
    refused(std::string(kMaxManifestBytes + 1, ' '), "");
    refused(with("version", "\"1.4\""), "version");
    refused(with("version", "\"v1.4.0\""), "version");
    refused(with("version", "140"), "version");
    refused(R"({"board":"simulator"})", "version");  // missing
    refused(with("board", "\"\""), "board");
    refused(with("board", "\"" + std::string(kMaxBoardLength + 1, 'b') + "\""), "board");
    refused(with("board", "7"), "board");
    refused(with("layout", "0"), "layout");
    refused(with("layout", "65536"), "layout");
    refused(with("layout", "1.5"), "layout");
    refused(with("layout", "\"1\""), "layout");
    refused(with("layout", "true"), "layout");
    refused(with("size", "0"), "size");
    refused(with("size", "-5"), "size");
    refused(with("size", std::to_string(kSlot + 1)), "size");
    refused(with("size", "1000.0"), "size");
    refused(with("sha256", "\"" + std::string(63, 'a') + "\""), "sha256");
    refused(with("sha256", "\"" + std::string(65, 'a') + "\""), "sha256");
    refused(with("sha256", "\"" + std::string(64, 'A') + "\""), "sha256");
    refused(with("sha256", "\"" + std::string(64, 'g') + "\""), "sha256");
    refused(with("min_version", "null"), "min_version");
    refused(with("min_version", "\"1.2\""), "min_version");
    refused(with("min_version", "120"), "min_version");
}

TEST(FirmwareAvailability, OnlyANewerReleaseForThisBoard) {
    EXPECT_EQ(availability(running("1.3.2"), offered("1.4.0")), Availability::Available);
    EXPECT_EQ(availability(running("1.3.2"), offered("1.3.2")), Availability::NotNewer);
    EXPECT_EQ(availability(running("1.3.2"), offered("1.3.1")), Availability::NotNewer);
    // A development build never sees a newer one.
    for (const char *dev : {"v1.3.2-4-gabc1234", "1.3.2-dirty", "unknown", "", "0.0.0-sim"}) {
        EXPECT_EQ(availability(running(dev), offered("99.0.0")), Availability::DevelopmentBuild) << dev;
    }
    Manifest other = offered("1.4.0");
    other.board = "another_board";
    EXPECT_EQ(availability(running(), other), Availability::WrongBoard);
    Manifest relaid = offered("1.4.0");
    relaid.layout = 2;
    EXPECT_EQ(availability(running(), relaid), Availability::WrongLayout);
    // min_version: the running one must be at least that.
    EXPECT_EQ(availability(running("1.1.0"), offered("1.4.0", "1.2.0")), Availability::NeedsNewerBase);
    EXPECT_EQ(availability(running("1.2.0"), offered("1.4.0", "1.2.0")), Availability::Available);
    EXPECT_STREQ(availabilityName(Availability::NeedsNewerBase), "needs_newer_base");
}

TEST(FirmwareImage, MatchesItsManifestsSizeAndHash) {
    auth::SoftwareHash hash;
    const std::string bin = sim::simulatorImage("1.4.0");
    sim::FakeReleaseFeed feed;
    feed.published = true;
    feed.version = "1.4.0";
    Manifest m;
    ASSERT_TRUE(parseManifest(feed.manifestJson(), kSlot, &m));
    EXPECT_EQ(checkImage(bin, m, hash), ImageCheck::Ok);
    EXPECT_EQ(checkImage(bin.substr(1), m, hash), ImageCheck::WrongSize);
    std::string flipped = bin;
    flipped[100] ^= 1;
    EXPECT_EQ(checkImage(flipped, m, hash), ImageCheck::WrongHash);
}

TEST(FirmwareCheck, FetchesVerifiesParsesAndDecides) {
    sim::FakeReleaseFeed feed;
    EXPECT_EQ(checkForUpdate(feed, running()).error, CheckError::Unreachable);  // no release yet
    feed.published = true;
    feed.version = "1.4.0";
    CheckOutcome found = checkForUpdate(feed, running("1.3.2"));
    EXPECT_EQ(found.error, CheckError::None);
    ASSERT_TRUE(found.available.has_value());
    EXPECT_EQ(found.available->version, "1.4.0");
    EXPECT_EQ(found.available->size, feed.image().size());

    const CheckOutcome same = checkForUpdate(feed, running("1.4.0"));
    EXPECT_EQ(same.error, CheckError::None);
    EXPECT_FALSE(same.available.has_value());
    EXPECT_EQ(same.availability, Availability::NotNewer);
    EXPECT_EQ(checkForUpdate(feed, running("v1.3.2-1-gabc")).availability, Availability::DevelopmentBuild);

    feed.fault = release::CheckError::BadSignature;
    EXPECT_EQ(checkForUpdate(feed, running()).error, CheckError::BadSignature);
    feed.fault = release::CheckError::BadManifest;
    EXPECT_EQ(checkForUpdate(feed, running()).error, CheckError::BadManifest);
    feed.fault = release::CheckError::Unreachable;
    EXPECT_EQ(checkForUpdate(feed, running()).error, CheckError::Unreachable);
    EXPECT_EQ(checkErrorName(CheckError::None), nullptr);
    EXPECT_STREQ(checkErrorName(CheckError::BadSignature), "bad_signature");

    // The image it serves passes its own manifest's check.
    feed.fault = release::CheckError::None;
    std::vector<uint8_t> buffer(kSlot);
    size_t length = 0;
    ASSERT_EQ(feed.fetchImage(*found.available, buffer.data(), buffer.size(), &length), FetchResult::Ok);
    auth::SoftwareHash hash;
    EXPECT_EQ(checkImage(std::string_view(reinterpret_cast<const char *>(buffer.data()), length), *found.available,
                         hash),
              ImageCheck::Ok);
}

TEST(FirmwareCheckState, KeepsTheLastReleaseThroughAnError) {
    UpdateCheckState state;
    EXPECT_FALSE(state.read().checked);
    ASSERT_TRUE(state.beginCheck());
    EXPECT_FALSE(state.beginCheck());  // one at a time
    EXPECT_TRUE(state.read().checking);
    CheckOutcome found;
    found.available = offered("1.4.0");
    found.availability = Availability::Available;
    state.finishCheck(found, 5000);
    UpdateCheckStatus s = state.read();
    EXPECT_FALSE(s.checking);
    EXPECT_TRUE(s.checked);
    EXPECT_EQ(s.checkedAtMs, 5000u);
    ASSERT_NE(s.available, nullptr);
    EXPECT_EQ(s.available->version, "1.4.0");
    EXPECT_TRUE(state.availableAs("1.4.0").has_value());
    EXPECT_FALSE(state.availableAs("1.3.0").has_value());

    // An error keeps it (it was signed, and is still the latest known).
    CheckOutcome failed;
    failed.error = CheckError::Unreachable;
    ASSERT_TRUE(state.beginCheck());
    state.finishCheck(failed, 9000);
    s = state.read();
    EXPECT_EQ(s.error, CheckError::Unreachable);
    EXPECT_EQ(s.checkedAtMs, 9000u);
    EXPECT_NE(s.available, nullptr);

    // A good check with nothing newer clears it, and the error.
    ASSERT_TRUE(state.beginCheck());
    state.finishCheck(CheckOutcome{}, 12000);
    s = state.read();
    EXPECT_EQ(s.error, CheckError::None);
    EXPECT_EQ(s.available, nullptr);

    state.setRolledBackFrom("1.5.0");
    EXPECT_EQ(state.read().rolledBackFrom, "1.5.0");
    state.clearRolledBackFrom();
    EXPECT_TRUE(state.read().rolledBackFrom.empty());
}

// The firmware's update task writes while httpd reads (tsan covers it).
TEST(FirmwareCheckState, OneWriterAndAReaderOnTwoTasks) {
    UpdateCheckState state;
    std::thread writer([&] {
        for (uint32_t i = 0; i < 2000; i++) {
            if (!state.beginCheck()) continue;
            CheckOutcome outcome;
            if (i % 2 == 0) outcome.available = offered("1.4.0");
            state.finishCheck(outcome, i);
        }
    });
    for (int i = 0; i < 2000; i++) {
        const UpdateCheckStatus s = state.read();
        if (s.available) {
            EXPECT_EQ(s.available->version, "1.4.0");
        }
    }
    writer.join();
}

// hub-sim's FirmwareReleasePort: Check now, then install, as one update with
// the upload path (Downloading, Writing, Rebooting), on hub time.
TEST(SimReleases, CheckThenInstallRunsTheWholeUpdate) {
    sim::SimHub hub;
    sim::SimFirmwareReleases &releases = hub.releases();
    api::SimulatedFirmwareUpdate &firmware = hub.firmware();
    releases.setVersion("1.3.0");
    releases.feed.published = true;
    releases.feed.version = "1.4.0";
    releases.state().setRolledBackFrom("1.3.1");

    EXPECT_EQ(releases.install("1.4.0"), api::ReleaseInstallResult::NotAvailable);  // not checked yet
    ASSERT_EQ(releases.requestCheck(), api::ReleaseCheckResult::Accepted);
    EXPECT_EQ(releases.requestCheck(), api::ReleaseCheckResult::Busy);
    EXPECT_TRUE(releases.releaseStatus().check.checking);
    hub.advance(sim::SimFirmwareReleases::kCheckMs);
    api::FirmwareReleaseStatus status = releases.releaseStatus();
    EXPECT_FALSE(status.check.checking);
    ASSERT_NE(status.check.available, nullptr);
    EXPECT_EQ(status.check.available->version, "1.4.0");
    hub.advance(250);
    EXPECT_EQ(releases.releaseStatus().checkedAgoMs, 250u);

    EXPECT_EQ(releases.install("1.3.9"), api::ReleaseInstallResult::NotAvailable);
    ASSERT_EQ(releases.install("1.4.0"), api::ReleaseInstallResult::Accepted);
    EXPECT_TRUE(releases.releaseStatus().check.rolledBackFrom.empty());  // a new update
    EXPECT_EQ(firmware.status().state, api::FirmwareUpdateState::Downloading);
    EXPECT_EQ(releases.install("1.4.0"), api::ReleaseInstallResult::Busy);
    EXPECT_EQ(firmware.install(sim::simulatorImage("1.4.0")), api::FirmwareInstallResult::Busy);  // no upload either
    hub.advance(sim::SimFirmwareReleases::kDownloadMs);
    EXPECT_EQ(firmware.status().state, api::FirmwareUpdateState::Writing);
    hub.advance(api::SimulatedFirmwareUpdate::kWriteMs + api::SimulatedFirmwareUpdate::kRebootMs);
    EXPECT_EQ(firmware.status().state, api::FirmwareUpdateState::Idle);
    EXPECT_EQ(releases.version(), "1.4.0");  // it runs the new image
    // A reboot forgets every check, as the board's RAM does.
    status = releases.releaseStatus();
    EXPECT_FALSE(status.check.checked);
    EXPECT_EQ(status.check.available, nullptr);
    EXPECT_EQ(status.check.error, CheckError::None);

    // Now up to date: the next check offers nothing.
    ASSERT_EQ(releases.requestCheck(), api::ReleaseCheckResult::Accepted);
    hub.advance(sim::SimFirmwareReleases::kCheckMs);
    EXPECT_EQ(releases.releaseStatus().check.available, nullptr);
}

TEST(SimReleases, AFailedDownloadEndsTheUpdate) {
    sim::SimHub hub;
    sim::SimFirmwareReleases &releases = hub.releases();
    releases.setVersion("1.3.0");
    releases.feed.published = true;
    releases.feed.version = "1.4.0";
    const auto installAfter = [&](const std::function<void()> &breakIt) {
        hub.advance(api::SimulatedFirmwareUpdate::kRebootMs);  // past any earlier failure's state
        ASSERT_EQ(releases.requestCheck(), api::ReleaseCheckResult::Accepted);
        hub.advance(sim::SimFirmwareReleases::kCheckMs);
        ASSERT_EQ(releases.install("1.4.0"), api::ReleaseInstallResult::Accepted);
        breakIt();
        hub.advance(sim::SimFirmwareReleases::kDownloadMs);
    };
    // The feed goes away mid-download.
    installAfter([&] { releases.feed.fault = release::CheckError::Unreachable; });
    EXPECT_EQ(hub.firmware().status().state, api::FirmwareUpdateState::Failed);
    EXPECT_EQ(hub.firmware().status().error, api::kDownloadFailed);
    // The feed moves on: the image isn't the signed manifest's.
    releases.feed.fault = release::CheckError::None;
    installAfter([&] { releases.feed.version = "1.4.1"; });
    EXPECT_EQ(hub.firmware().status().error, api::kImageMismatch);
    // Signed and matching, but for another board: refused as an upload is.
    releases.feed.version = "1.4.0";
    releases.feed.board = "another_board";
    hub.advance(api::SimulatedFirmwareUpdate::kRebootMs);
    ASSERT_EQ(releases.requestCheck(), api::ReleaseCheckResult::Accepted);
    hub.advance(sim::SimFirmwareReleases::kCheckMs);
    EXPECT_EQ(releases.releaseStatus().check.error, CheckError::None);
    EXPECT_EQ(releases.releaseStatus().check.available, nullptr);  // wrong_board: never offered
    std::string bin = sim::simulatorImage("1.4.0", "another_board");
    ASSERT_TRUE(hub.firmware().beginDownload());
    EXPECT_EQ(hub.firmware().finishDownload(bin), api::FirmwareInstallResult::WrongBoard);
    EXPECT_EQ(hub.firmware().status().error, api::kInvalidImage);
}
