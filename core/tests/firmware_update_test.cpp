// Firmware image validation (docs/security.md, "Firmware updates") and
// SimulatedFirmwareUpdate's states.
#include <gtest/gtest.h>

#include "firmware_image_util.h"
#include "hubbo/api/firmware_update_port.h"

using namespace hubbo;
using namespace hubbo::api;
using R = FirmwareInstallResult;
using hubbo::test::image;
using hubbo::test::ImageSpec;

namespace {

R validate(const std::string &bytes, std::string *version = nullptr) {
    auth::SoftwareHash hash;
    return validateFirmwareImage(bytes, simulatorFirmwareTarget(), hash, version);
}

}  // namespace

TEST(FirmwareImage, AcceptsAHubboImageAndReadsItsVersion) {
    std::string version;
    EXPECT_EQ(validate(image(), &version), R::Accepted);
    EXPECT_EQ(version, "v1.2.3-4-gabc");
}

TEST(FirmwareImage, RefusesEachMistakeInOrder) {
    ImageSpec big;
    big.padTo = simulatorFirmwareTarget().slotBytes;  // + 32 hash bytes: over the slot
    EXPECT_EQ(validate(image(big)), R::TooLarge);
    EXPECT_EQ(validate("not an image"), R::NotAnImage);
    ImageSpec s;
    s.magic = 0x00;
    EXPECT_EQ(validate(image(s)), R::NotAnImage);
    s = {};
    s.chip = 2;  // ESP32-S2
    EXPECT_EQ(validate(image(s)), R::WrongChip);
    s = {};
    s.minRev = 3;  // needs v0.3; the target is v0.2
    EXPECT_EQ(validate(image(s)), R::WrongChip);
    s.minRev = 2;
    EXPECT_EQ(validate(image(s)), R::Accepted);
    s = {};
    s.descMagic = 0;  // a merged factory .bin: the bootloader comes first
    EXPECT_EQ(validate(image(s)), R::NotAnImage);
    s = {};
    s.marker = "";
    EXPECT_EQ(validate(image(s)), R::ForeignImage);
    s.marker = "HUBBO-IMAGE/1 board=simulator layout=x version=1";
    EXPECT_EQ(validate(image(s)), R::ForeignImage);
    s.marker = "HUBBO-IMAGE/1 board=waveshare_esp32_s3_eth layout=1 version=1";
    EXPECT_EQ(validate(image(s)), R::WrongBoard);
    s.marker = "HUBBO-IMAGE/1 board=simulator layout=2 version=1";
    EXPECT_EQ(validate(image(s)), R::WrongLayout);
    s = {};
    s.hashAppended = false;
    EXPECT_EQ(validate(image(s)), R::Corrupt);
    s = {};
    s.badHash = true;
    EXPECT_EQ(validate(image(s)), R::Corrupt);
    std::string flipped = image();
    flipped[400] ^= 1;  // one byte changed in transit
    EXPECT_EQ(validate(flipped), R::Corrupt);
}

TEST(SimulatedFirmwareUpdate, WritesRebootsAndEndsIdle) {
    SimulatedFirmwareUpdate fw;
    EXPECT_EQ(fw.status().state, FirmwareUpdateState::Idle);
    ImageSpec wrong;
    wrong.badHash = true;
    EXPECT_EQ(fw.install(image(wrong)), R::Corrupt);
    EXPECT_EQ(fw.status().state, FirmwareUpdateState::Idle);  // a refused image changes nothing

    EXPECT_EQ(fw.install(image()), R::Accepted);
    EXPECT_EQ(fw.lastVersion(), "v1.2.3-4-gabc");
    EXPECT_EQ(fw.status().state, FirmwareUpdateState::Writing);
    EXPECT_EQ(fw.install(image()), R::Busy);
    fw.advance(SimulatedFirmwareUpdate::kWriteMs / 2);
    EXPECT_EQ(fw.status().progress, 50);
    fw.advance(SimulatedFirmwareUpdate::kWriteMs / 2);
    EXPECT_EQ(fw.status().state, FirmwareUpdateState::Rebooting);
    EXPECT_EQ(fw.install(image()), R::Busy);
    fw.advance(SimulatedFirmwareUpdate::kRebootMs);
    EXPECT_EQ(fw.status().state, FirmwareUpdateState::Idle);
    EXPECT_STREQ(firmwareUpdateStateName(fw.status().state), "idle");
}

TEST(SimulatedFirmwareUpdate, ADownloadWaitsForItsImage) {
    SimulatedFirmwareUpdate fw;
    ASSERT_TRUE(fw.beginDownload());
    EXPECT_FALSE(fw.beginDownload());
    EXPECT_EQ(fw.install(image()), R::Busy);
    // Time passing doesn't end a download; only its image (or a failure) does.
    fw.advance(SimulatedFirmwareUpdate::kWriteMs + SimulatedFirmwareUpdate::kRebootMs);
    EXPECT_EQ(fw.status().state, FirmwareUpdateState::Downloading);
    ASSERT_EQ(fw.finishDownload(image()), R::Accepted);
    EXPECT_EQ(fw.status().state, FirmwareUpdateState::Writing);
    EXPECT_EQ(fw.status().progress, 0);
    fw.advance(SimulatedFirmwareUpdate::kWriteMs);
    EXPECT_EQ(fw.status().state, FirmwareUpdateState::Rebooting);

    SimulatedFirmwareUpdate failing;
    ASSERT_TRUE(failing.beginDownload());
    failing.failDownload(kDownloadFailed);
    EXPECT_EQ(failing.status().state, FirmwareUpdateState::Failed);
    EXPECT_EQ(failing.status().error, "download_failed");
    ASSERT_TRUE(failing.beginDownload());
    ImageSpec bad;
    bad.badHash = true;
    EXPECT_NE(failing.finishDownload(image(bad)), R::Accepted);
    EXPECT_EQ(failing.status().error, "invalid_image");
}

TEST(SimulatedFirmwareUpdate, FailedWriteAllowsAnotherTry) {
    SimulatedFirmwareUpdate fw;
    fw.failWrites = true;
    ASSERT_EQ(fw.install(image()), R::Accepted);
    fw.advance(SimulatedFirmwareUpdate::kWriteMs);
    EXPECT_EQ(fw.status().state, FirmwareUpdateState::Failed);
    EXPECT_EQ(fw.status().error, "write_failed");
    fw.failWrites = false;
    EXPECT_EQ(fw.install(image()), R::Accepted);
    fw.ready = false;
    fw.advance(SimulatedFirmwareUpdate::kWriteMs + SimulatedFirmwareUpdate::kRebootMs);
    EXPECT_EQ(fw.install(image()), R::Unavailable);
}
