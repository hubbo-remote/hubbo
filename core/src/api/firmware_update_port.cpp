#include "hubbo/api/firmware_update_port.h"

#include <cstring>

#include "hubbo/domain/limits.h"
#include "support.h"

namespace hubbo::api {

namespace {

// esp_image_header_t (24 bytes), then the first segment's header (8), then
// that segment's esp_app_desc_t, whose first word is its magic.
constexpr size_t kHeaderBytes = 24;
constexpr size_t kChipIdOffset = 12;       // uint16 LE
constexpr size_t kMinChipRevOffset = 15;   // min_chip_rev_full, uint16 LE
constexpr size_t kHashAppendedOffset = 23;
constexpr size_t kAppDescOffset = kHeaderBytes + 8;
constexpr uint32_t kAppDescMagic = 0xABCD5432;
constexpr uint8_t kImageMagic = 0xE9;

uint16_t le16(std::string_view b, size_t at) {
    return static_cast<uint16_t>(static_cast<uint8_t>(b[at]) | static_cast<uint8_t>(b[at + 1]) << 8);
}

uint32_t le32(std::string_view b, size_t at) {
    return uint32_t{le16(b, at)} | uint32_t{le16(b, at + 2)} << 16;
}

// "<key>=<value>" up to `end` (' ' or NUL) at the front of `text`; consumes it.
bool takeField(std::string_view &text, std::string_view key, char end, std::string_view *value) {
    if (text.substr(0, key.size()) != key || text.size() <= key.size() || text[key.size()] != '=') return false;
    text.remove_prefix(key.size() + 1);
    const size_t stop = text.find(end);
    if (stop == 0 || stop == std::string_view::npos) return false;
    *value = text.substr(0, stop);
    text.remove_prefix(stop + (end == ' ' ? 1 : 0));
    return true;
}

bool printable(std::string_view text) {
    for (char c : text) {
        if (c < 0x21 || c > 0x7e) return false;
    }
    return true;
}

struct Marker {
    std::string_view board, version;
    uint64_t layout = 0;
};

// The first well-formed marker. Earlier matches can be other strings that
// start the same way (the validator's own prefix literal is in every image).
bool findMarker(std::string_view image, Marker *out) {
    const std::string_view prefix = kFirmwareMarkerPrefix;
    for (size_t at = image.find(prefix); at != std::string_view::npos; at = image.find(prefix, at + 1)) {
        std::string_view rest = image.substr(at + prefix.size(), 160);
        std::string_view board, layout, version;
        if (!takeField(rest, "board", ' ', &board) || !takeField(rest, "layout", ' ', &layout) ||
            !takeField(rest, "version", '\0', &version)) {
            continue;
        }
        if (!printable(board) || version.size() > limits::kMaxFirmwareVersionLength || !printable(version) ||
            !parseDecimal(layout, 9999, &out->layout)) {
            continue;
        }
        out->board = board;
        out->version = version;
        return true;
    }
    return false;
}

}  // namespace

FirmwareInstallResult validateFirmwareImage(std::string_view image, const FirmwareTarget &target, auth::Hash &hash,
                                            std::string *version) {
    using R = FirmwareInstallResult;
    if (image.size() > target.slotBytes) return R::TooLarge;
    if (image.size() < kAppDescOffset + 4 + auth::kSha256Size || static_cast<uint8_t>(image[0]) != kImageMagic) {
        return R::NotAnImage;
    }
    if (le16(image, kChipIdOffset) != kEsp32S3ChipId || le16(image, kMinChipRevOffset) > target.chipRevision) {
        return R::WrongChip;
    }
    if (le32(image, kAppDescOffset) != kAppDescMagic) return R::NotAnImage;
    Marker marker;
    if (!findMarker(image, &marker)) return R::ForeignImage;
    if (marker.board != target.board) return R::WrongBoard;
    if (marker.layout != target.layout) return R::WrongLayout;
    if (image[kHashAppendedOffset] != 1) return R::Corrupt;
    const size_t body = image.size() - auth::kSha256Size;
    uint8_t digest[auth::kSha256Size];
    hash.sha256(reinterpret_cast<const uint8_t *>(image.data()), body, digest);
    if (std::memcmp(digest, image.data() + body, auth::kSha256Size) != 0) return R::Corrupt;
    if (version != nullptr) version->assign(marker.version);
    return R::Accepted;
}

const char *firmwareUpdateStateName(FirmwareUpdateState state) {
    switch (state) {
        case FirmwareUpdateState::Downloading: return "downloading";
        case FirmwareUpdateState::Writing: return "writing";
        case FirmwareUpdateState::Rebooting: return "rebooting";
        case FirmwareUpdateState::Failed: return "failed";
        case FirmwareUpdateState::Idle: break;
    }
    return "idle";
}

FirmwareTarget simulatorFirmwareTarget() { return FirmwareTarget{"simulator", 1, 2, 64 * 1024}; }

bool SimulatedFirmwareUpdate::beginDownload() {
    if (firmwareUpdateRunning(status_.state)) return false;
    status_ = FirmwareUpdateStatus{FirmwareUpdateState::Downloading, 0, ""};
    return true;
}

FirmwareInstallResult SimulatedFirmwareUpdate::finishDownload(std::string_view image) {
    if (status_.state != FirmwareUpdateState::Downloading) return FirmwareInstallResult::Busy;
    status_ = FirmwareUpdateStatus{};
    const FirmwareInstallResult result = install(image);
    if (result != FirmwareInstallResult::Accepted) failDownload(kInvalidImage);
    return result;
}

void SimulatedFirmwareUpdate::failDownload(const char *error) {
    status_ = FirmwareUpdateStatus{FirmwareUpdateState::Failed, 0, error};
}

FirmwareInstallResult SimulatedFirmwareUpdate::install(std::string_view image) {
    if (firmwareUpdateRunning(status_.state)) return FirmwareInstallResult::Busy;
    if (!ready) return FirmwareInstallResult::Unavailable;
    auth::SoftwareHash hash;
    std::string version;
    const FirmwareInstallResult result = validateFirmwareImage(image, target_, hash, &version);
    if (result != FirmwareInstallResult::Accepted) return result;
    lastVersion_ = std::move(version);
    status_ = FirmwareUpdateStatus{FirmwareUpdateState::Writing, 0, ""};
    elapsedMs_ = 0;
    return result;
}

void SimulatedFirmwareUpdate::advance(uint32_t elapsedMs) {
    // A download is the release port's to time: only the write and the reboot run here.
    if (status_.state != FirmwareUpdateState::Writing && status_.state != FirmwareUpdateState::Rebooting) return;
    elapsedMs_ += elapsedMs;
    if (status_.state == FirmwareUpdateState::Writing) {
        if (elapsedMs_ < kWriteMs) {
            status_.progress = static_cast<uint8_t>(uint64_t{elapsedMs_} * 100 / kWriteMs);
            return;
        }
        elapsedMs_ -= kWriteMs;
        if (failWrites) {
            status_ = FirmwareUpdateStatus{FirmwareUpdateState::Failed, 0, "write_failed"};
            return;
        }
        status_ = FirmwareUpdateStatus{FirmwareUpdateState::Rebooting, 100, ""};
    }
    if (elapsedMs_ >= kRebootMs) status_ = FirmwareUpdateStatus{};
}

}  // namespace hubbo::api
