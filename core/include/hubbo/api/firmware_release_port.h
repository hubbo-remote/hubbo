#pragma once

#include <cstdint>
#include <string_view>

#include "hubbo/api/firmware_update_port.h"
#include "hubbo/integrations/firmware_release.h"

// Online firmware updates as the admin API sees them
// (docs/design/firmware-updates.md): the release fields of GET
// /admin/firmware, POST /admin/firmware/check and POST
// /admin/firmware/install.
//
// The firmware implements the port with its update task (a check and a
// download block on the network, so they run there, never on httpd or the RF
// task); hub-sim implements it over its fake feed (/sim/release). Every
// method runs on the HTTP task and returns quickly: it reads state or hands
// work off.
//
// One update at a time, shared with FirmwareUpdatePort: an install's
// download shows there as FirmwareUpdateState::Downloading, then the image
// goes through the same validation and write as an upload (Writing,
// Rebooting). While any update or reboot is running, install() is Busy, and
// an upload is Busy too.
namespace hubbo::api {

// GET /admin/firmware's release fields.
struct FirmwareReleaseStatus {
    release::UpdateCheckStatus check;  // checking, checked, error, available, rolledBackFrom
    uint32_t checkedAgoMs = 0;         // now - check.checkedAtMs on the writer's clock; when check.checked
};

enum class ReleaseCheckResult : uint8_t {
    Accepted,  // a check has started (status shows checking until it ends)
    Busy,      // a check is already running (UpdateCheckState::beginCheck() said no)
};

enum class ReleaseInstallResult : uint8_t {
    Accepted,      // the download has started: the update port's status is Downloading
    NotAvailable,  // `version` isn't the available release (UpdateCheckState::availableAs()): 409 not_available
    Busy,          // an update or reboot is running (checked first): 409 update_in_progress
};

// A failed install ends in the update port's Failed state with
// kDownloadFailed, kImageMismatch or kInvalidImage (firmware_update_port.h).

class FirmwareReleasePort {
public:
    virtual ~FirmwareReleasePort() = default;

    virtual FirmwareReleaseStatus releaseStatus() const = 0;
    // Start a check now (Check now). Runs whether or not "Check for updates"
    // is on.
    virtual ReleaseCheckResult requestCheck() = 0;
    // Install the available release `version`: Busy first, then NotAvailable,
    // else hand the download to the update task and clear rolledBackFrom.
    virtual ReleaseInstallResult install(std::string_view version) = 0;
};

}  // namespace hubbo::api
