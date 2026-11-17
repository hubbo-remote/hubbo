#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "hubbo/auth/crypto.h"

// Online firmware updates, the portable side (docs/design/firmware-updates.md):
// the signed release manifest, semver, whether a release is available to this
// hub, the update-check state the API reads, and the ReleaseFeed port that
// fetches and verifies (the firmware: esp_http_client and mbedTLS; hub-sim: a
// fake feed set through /sim/release).
//
// Nothing here runs on the RF/engine task. A check blocks on the network
// (the feed), so it runs on the firmware's update task (hub-sim: inline).
namespace hubbo::release {

constexpr size_t kMaxManifestBytes = 4096;  // the whole manifest.json (CI's notes, up to 2 KB, included)
constexpr size_t kMaxBoardLength = 32;
constexpr size_t kSha256HexLength = 64;

// ---------------------------------------------------------------- versions

// A release version: plain "X.Y.Z", each part decimal digits with no
// leading zero (except "0"), at most 65535. Nothing else: no "v", no
// pre-release or build suffix.
struct Version {
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t patch = 0;
};

// False for anything that isn't a plain X.Y.Z: a development build's git
// describe ("v1.4.0-3-gabc1234", "1.4.0-dirty"), "unknown", "".
bool parseVersion(std::string_view text, Version *out);
// <0, 0 or >0, as a is older than, the same as, or newer than b.
int compareVersions(const Version &a, const Version &b);

// ---------------------------------------------------------------- manifest

// manifest.json:
//   {"version","board","layout","size","sha256","min_version"?}
// (CI also writes "notes"; the hub links to the GitHub release instead, and
// ignores it as any unknown field.)
struct Manifest {
    std::string version;     // plain X.Y.Z
    std::string board;       // e.g. "waveshare_esp32_s3_eth" (HubInfo::board, the image marker's)
    uint16_t layout = 0;     // the partition layout (the image marker's layout=)
    size_t size = 0;         // the image's bytes, 1..the OTA slot
    std::string sha256;      // the image's, 64 lowercase hex
    std::string minVersion;  // the oldest version that can install it directly; "" when absent
};

// Strict: an object of at most kMaxManifestBytes; every field above of its
// type and within its limits (`size` at most `slotBytes`); min_version absent
// or a plain X.Y.Z. Unknown fields are ignored (a later release may add one;
// this hub must still read it). False with `badField` the first bad field's
// name ("" when it isn't a JSON object or is too big).
bool parseManifest(std::string_view json, size_t slotBytes, Manifest *out, std::string *badField = nullptr);

// ---------------------------------------------------------------- availability

enum class Availability : uint8_t {
    Available,         // for this board and layout, newer, and installable from the running one
    DevelopmentBuild,  // the running version isn't a plain X.Y.Z: never offered anything
    WrongBoard,
    WrongLayout,
    NotNewer,          // the same as the running one, or older
    NeedsNewerBase,    // the running one is older than its min_version
};
// "available", "development_build", "wrong_board", "wrong_layout", "not_newer", "needs_newer_base"
const char *availabilityName(Availability availability);

// What this hub is running, and what an image must match.
struct RunningFirmware {
    std::string version;  // HubInfo::version
    std::string board;    // HubInfo::board
    uint16_t layout = 1;
    size_t slotBytes = 0;  // the inactive OTA slot's size
};

// Whether `manifest` (already parsed) is an update for `running`, in this
// order: DevelopmentBuild, WrongBoard, WrongLayout, NotNewer, NeedsNewerBase.
Availability availability(const RunningFirmware &running, const Manifest &manifest);

// `data`'s SHA-256 in lowercase hex (as a manifest's sha256).
std::string sha256Hex(std::string_view data, auth::Hash &hash);

// A downloaded image against its signed manifest: its size, then its sha256.
enum class ImageCheck : uint8_t { Ok, WrongSize, WrongHash };
ImageCheck checkImage(std::string_view image, const Manifest &manifest, auth::Hash &hash);

// ---------------------------------------------------------------- the feed port

enum class FetchResult : uint8_t {
    Ok,
    Unreachable,   // no connection, an HTTP error, a timeout, or bigger than allowed
    BadSignature,  // manifest.sig doesn't verify over manifest.json's bytes (manifest only)
};

// The release feed: GitHub's latest release on the board, a fake in hub-sim.
// Both calls block on the network: the update task only, never the RF task or
// httpd. One connection at a time.
class ReleaseFeed {
public:
    virtual ~ReleaseFeed() = default;
    // manifest.json and manifest.sig; the signature (ECDSA P-256, the built-in
    // public key) verified over the manifest's exact bytes. Ok: `*manifest`
    // holds those bytes (at most kMaxManifestBytes, else Unreachable).
    virtual FetchResult fetchManifest(std::string *manifest) = 0;
    // hubbo-<manifest.version>.bin into `buffer` (the caller's: PSRAM on the
    // board), at most `capacity` bytes (else Unreachable). Ok: `*length` bytes
    // written. The caller then checkImage()s it against the manifest.
    virtual FetchResult fetchImage(const Manifest &manifest, uint8_t *buffer, size_t capacity, size_t *length) = 0;
};

// ---------------------------------------------------------------- one check

enum class CheckError : uint8_t { None, Unreachable, BadSignature, BadManifest };
// nullptr (JSON null) for None; "unreachable", "bad_signature", "bad_manifest"
const char *checkErrorName(CheckError error);

struct CheckOutcome {
    CheckError error = CheckError::None;
    // The manifest, when it's Available to this hub; else empty (an error, or
    // nothing newer).
    std::optional<Manifest> available;
    Availability availability = Availability::NotNewer;  // why not, when no error and none available
};

// Fetch, verify, parse, decide. Blocks on the feed.
CheckOutcome checkForUpdate(ReleaseFeed &feed, const RunningFirmware &running);

// An install's download: the feed's image into `buffer` (at most
// `capacity`), then checkImage(). Failed: the feed didn't deliver it
// (kDownloadFailed); Mismatch: not the signed manifest's (kImageMismatch).
// Blocks on the feed. Ok: `*length` bytes, ready for the upload path.
enum class DownloadResult : uint8_t { Ok, Failed, Mismatch };
DownloadResult downloadImage(ReleaseFeed &feed, const Manifest &manifest, uint8_t *buffer, size_t capacity,
                             auth::Hash &hash, size_t *length);

// ---------------------------------------------------------------- the state the API reads

struct UpdateCheckStatus {
    bool checking = false;     // a check is running (Check now while one runs joins it)
    bool checked = false;      // a check has finished since boot
    uint32_t checkedAtMs = 0;  // when, on the writer's millis clock: checked_ago_ms = now - checkedAtMs
    CheckError error = CheckError::None;  // the last check's
    // The latest available release. A check that errors keeps the one
    // before (it was signed and is still the latest known); a check that
    // succeeds replaces it (null: nothing newer). Shared, so a read (every
    // GET /admin/firmware poll) copies a pointer, not the manifest.
    std::shared_ptr<const Manifest> available;
    std::string rolledBackFrom;  // the version the bootloader rolled back from, until the next update; ""
};

// Written by the update task (the firmware's; hub-sim's single task), read by
// httpd. A mutex: neither side is the RF task, and each hold is a copy.
class UpdateCheckState {
public:
    UpdateCheckStatus read() const;
    // False if a check is already running (the caller doesn't start another).
    bool beginCheck();
    void finishCheck(CheckOutcome outcome, uint32_t nowMs);
    // A reboot's (the RAM is gone): never checked, nothing available.
    // rolledBackFrom stays: the boot sets it.
    void forgetChecks();
    // At boot, after a rollback; cleared when an update starts.
    void setRolledBackFrom(std::string_view version);
    void clearRolledBackFrom();
    // The available release, if its version is `version` (POST install's
    // {version}): what the installer downloads and checks against.
    std::optional<Manifest> availableAs(std::string_view version) const;

private:
    mutable std::mutex mutex_;
    UpdateCheckStatus status_;
};

}  // namespace hubbo::release
