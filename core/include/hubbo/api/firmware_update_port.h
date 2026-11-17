#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "hubbo/auth/crypto.h"

// Firmware updates as the admin API sees them (docs/design/phase5-appliance.md,
// section 2): GET /admin/firmware and POST /admin/firmware/update.
//
// The transport buffers the whole image (the board: PSRAM, through the normal
// body path), the route calls install(), and nothing is erased until the
// whole image has passed validateFirmwareImage(). There's no rollback.
//
// The firmware implements the port over esp_ota (a one-shot writer task,
// then requestReboot()); hub-sim uses SimulatedFirmwareUpdate below. Like
// RemotePort, every method runs on the HTTP task and returns quickly:
// install() validates (milliseconds plus one SHA-256 pass) and hands off.
namespace hubbo::api {

// What an image must match to be installed on this hub.
struct FirmwareTarget {
    std::string board;          // the marker's board=, e.g. "waveshare_esp32_s3_eth" (HubInfo::board)
    uint16_t layout = 1;        // the marker's layout= (partition table version; OTA can't change it)
    uint16_t chipRevision = 0;  // this chip's revision, major * 100 + minor (efuse); v0.2 = 2
    size_t slotBytes = 0;       // the inactive OTA slot's size
};

// The marker every Hubbo image carries (a NUL-terminated string in .rodata):
//   "HUBBO-IMAGE/1 board=<board> layout=<n> version=<git describe>"
constexpr const char *kFirmwareMarkerPrefix = "HUBBO-IMAGE/1 ";
constexpr uint16_t kEsp32S3ChipId = 9;  // esp_image_header_t.chip_id

enum class FirmwareInstallResult : uint8_t {
    Accepted,      // valid; the write has started (status() shows Writing)
    Busy,          // an update is running (this route's, or ArduinoOTA's), or the hub is rebooting
    Unavailable,   // can't take an update now (no memory for the copy, OTA not ready)
    TooLarge,      // bigger than the inactive slot
    NotAnImage,    // no 0xE9 magic or app description: not an app image (a merged factory .bin isn't one)
    WrongChip,     // not ESP32-S3, or needs a newer chip revision
    ForeignImage,  // no Hubbo marker: someone else's app
    WrongBoard,    // a Hubbo image for another board
    WrongLayout,   // a Hubbo image for another partition layout
    Corrupt,       // no appended SHA-256, or it doesn't match
};

// Validation steps 1-6 of the design, in order, on the whole buffered image;
// the first failure wins. `hash` computes the SHA-256 (the board may pass a
// hardware one). On success, `version` (if given) gets the marker's version.
// Returns Accepted or TooLarge..Corrupt, never Busy or Unavailable.
FirmwareInstallResult validateFirmwareImage(std::string_view image, const FirmwareTarget &target, auth::Hash &hash,
                                            std::string *version = nullptr);

enum class FirmwareUpdateState : uint8_t {
    Idle,         // nothing running (also after a reboot into the new image)
    Downloading,  // a release install is downloading the image (firmware_release_port.h)
    Writing,      // the writer task is erasing and writing the inactive slot
    Rebooting,  // written and set to boot; the hub restarts shortly
    Failed,     // the latest update stopped; the running image is untouched as the boot image
};
// "idle", "downloading", "writing", "rebooting", "failed"
const char *firmwareUpdateStateName(FirmwareUpdateState state);
// Downloading, Writing or Rebooting: no other update, and no factory reset
// or restart, may start.
inline bool firmwareUpdateRunning(FirmwareUpdateState state) {
    return state == FirmwareUpdateState::Downloading || state == FirmwareUpdateState::Writing ||
           state == FirmwareUpdateState::Rebooting;
}

// Failed `error` codes a release install adds (firmware_release_port.h):
// the feed didn't deliver the image (FetchResult not Ok); its size or sha256
// isn't the signed manifest's (release::checkImage()); it failed
// validateFirmwareImage(), the same rules as an upload.
constexpr const char *kDownloadFailed = "download_failed";
constexpr const char *kImageMismatch = "image_mismatch";
constexpr const char *kInvalidImage = "invalid_image";

struct FirmwareUpdateStatus {
    FirmwareUpdateState state = FirmwareUpdateState::Idle;
    uint8_t progress = 0;  // Downloading: percent received (0 if unknown); Writing: percent written; 0..100
    std::string error;     // Failed: a short code ("write_failed", "verify_failed", ...), else ""
};

class FirmwareUpdatePort {
public:
    virtual ~FirmwareUpdatePort() = default;

    virtual FirmwareUpdateStatus status() const = 0;
    // Validate `image` (validateFirmwareImage() against this hub's target)
    // and, if it passes, start writing it. `image` is valid only during the
    // call (the transport frees its buffer after the response): keep a copy.
    // Busy is checked first, so a second upload never disturbs a running one.
    virtual FirmwareInstallResult install(std::string_view image) = 0;
};

// The FirmwareTarget hub-sim uses: board "simulator" (its HubInfo::board),
// layout 1, chip revision v0.2, a 64 KiB slot (so a TooLarge test is cheap).
FirmwareTarget simulatorFirmwareTarget();

// In-memory updates for hub-sim and tests. install() runs the real
// validation; a write then takes kWriteMs of advance() time, then Rebooting
// lasts kRebootMs and ends in Idle (the simulated reboot). Single task.
class SimulatedFirmwareUpdate : public FirmwareUpdatePort {
public:
    static constexpr uint32_t kWriteMs = 2000;
    static constexpr uint32_t kRebootMs = 1000;

    explicit SimulatedFirmwareUpdate(FirmwareTarget target = simulatorFirmwareTarget())
        : target_(std::move(target)) {}
    FirmwareUpdateStatus status() const override { return status_; }
    FirmwareInstallResult install(std::string_view image) override;

    // A release install's download (hub-sim's FirmwareReleasePort):
    // beginDownload() is false while an update runs, else the state is
    // Downloading; then finishDownload() with the image (validated as an
    // upload: Writing, or Failed "invalid_image") or failDownload(error).
    bool beginDownload();
    FirmwareInstallResult finishDownload(std::string_view image);
    void failDownload(const char *error);

    // Simulation controls.
    void advance(uint32_t elapsedMs);
    bool ready = true;        // false: install() answers Unavailable
    bool failWrites = false;  // true: a write ends in Failed ("write_failed") instead of Rebooting
    const std::string &lastVersion() const { return lastVersion_; }  // the latest accepted image's

private:
    FirmwareTarget target_;
    FirmwareUpdateStatus status_;
    uint32_t elapsedMs_ = 0;  // in the current Writing or Rebooting state
    std::string lastVersion_;
};

}  // namespace hubbo::api
