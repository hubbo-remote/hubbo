#include "firmware_releases.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>

#include <optional>
#include <utility>

#include "board.h"
#include "console.h"
#include "ethernet_link.h"
#include "image_marker.h"

namespace hubbo::fw {

using hubbo::api::ReleaseCheckResult;
using hubbo::api::ReleaseInstallResult;
using hubbo::release::CheckOutcome;

namespace {

constexpr uint32_t kSelfRequestTimeoutMs = 3000;

bool pendingVerify() {
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

}  // namespace

void FirmwareReleases::boot(bool configLoaded) {
    configLoaded_ = configLoaded;
    const std::string running = image::version();
    const bool trial = pendingVerify();
    std::string pending;
    std::string rolledBack;
    const bool hasPending = store_.get(kPendingKey, &pending) == ErrorCode::Ok;
    const bool hasRolledBack = store_.get(kRolledBackKey, &rolledBack) == ErrorCode::Ok;

    const bool rolledBackNow = hasPending && pending != running;  // the bootloader didn't keep the image we wrote
    if (rolledBackNow) {
        con.printf("update: %s didn't start; back on %s\n", pending.c_str(), running.c_str());
        state_.setRolledBackFrom(pending);
        hooks_.postToHttpd([this, pending] {
            store_.set(kRolledBackKey, pending);
            store_.remove(kPendingKey);
        });
    } else if (hasRolledBack) {
        state_.setRolledBackFrom(rolledBack);
    }
    if (hasPending && !rolledBackNow && !trial) {
        con.printf("update: %s started without a trial (this bootloader doesn't roll back)\n", running.c_str());
        hooks_.postToHttpd([this] { store_.remove(kPendingKey); });
    }
    if (trial) {
        onTrial_ = true;
        firmware_.setTrial(true);
        con.printf("update: %s on trial; marked valid once healthy, else back to the previous image after %lu s\n",
                   running.c_str(), static_cast<unsigned long>(kTrialMs / 1000));
    }
}

FirmwareReleases::FirmwareReleases(FirmwareUpdate &firmware, hubbo::storage::StateStore &store,
                                   const hubbo::config::HubSettings &settings, const EthernetLink &ethernet,
                                   hubbo::auth::Hash &hash, Hooks hooks)
    : firmware_(firmware),
      store_(store),
      settings_(settings),
      ethernet_(ethernet),
      hooks_(std::move(hooks)),
      feed_(hash),
      hash_(hash) {
    feed_.setOnProgress([this](uint8_t percent) { firmware_.setDownloadProgress(percent); });
    firmware_.setOnWritten([this](const std::string &version) { onWritten(version); });
}

bool FirmwareReleases::start() {
    return task_.start("update", [this] { return service(); }, kHealthEveryMs);
}

void FirmwareReleases::onWritten(const std::string &written) {
    hooks_.postToHttpd([this, written] {
        store_.set(kPendingKey, written);
        store_.remove(kRolledBackKey);
        state_.clearRolledBackFrom();
    });
}

hubbo::api::FirmwareReleaseStatus FirmwareReleases::releaseStatus() const {
    hubbo::api::FirmwareReleaseStatus status;
    status.check = state_.read();
    if (status.check.checked) {
        status.checkedAgoMs = millis() - status.check.checkedAtMs;
    }
    return status;
}

ReleaseCheckResult FirmwareReleases::requestCheck() {
    if (!state_.beginCheck()) {
        return ReleaseCheckResult::Busy;
    }
    if (!task_.post([this] { runCheck(); })) {
        CheckOutcome outcome;
        outcome.error = hubbo::release::CheckError::Unreachable;
        state_.finishCheck(outcome, millis());
    }
    return ReleaseCheckResult::Accepted;
}

ReleaseInstallResult FirmwareReleases::install(std::string_view version) {
    if (hubbo::api::firmwareUpdateRunning(firmware_.status().state) || firmware_.trial()) {
        return ReleaseInstallResult::Busy;
    }
    std::optional<hubbo::release::Manifest> manifest = state_.availableAs(version);
    if (!manifest) {
        return ReleaseInstallResult::NotAvailable;
    }
    if (!firmware_.beginDownload()) {
        return ReleaseInstallResult::Busy;
    }
    state_.clearRolledBackFrom();
    store_.remove(kRolledBackKey);
    if (!task_.post([this, m = std::move(*manifest)]() mutable { download(std::move(m)); })) {
        firmware_.failDownload(hubbo::api::kDownloadFailed);
    }
    return ReleaseInstallResult::Accepted;
}

bool FirmwareReleases::setTest(std::string baseUrl, std::string runningVersion) {
    return task_.post([this, baseUrl = std::move(baseUrl), runningVersion = std::move(runningVersion)]() mutable {
        feed_.setTestFeed(std::move(baseUrl));
        versionOverride_ = std::move(runningVersion);
        con.println("update: test feed set");
    });
}

bool FirmwareReleases::service() {
    const uint32_t now = millis();
    if (onTrial_ && static_cast<int32_t>(now - nextHealthMs_) >= 0) {
        nextHealthMs_ = now + kHealthEveryMs;
        checkHealth(now);
    }
    // Not on trial: a check blocks this task (up to 30 s) and with it the health checks.
    if (!onTrial_ && static_cast<int32_t>(now - nextCheckMs_) >= 0 && ethernet_.up()) {
        nextCheckMs_ = now + kCheckEveryMs;
        if (settings_.checkUpdates() && state_.beginCheck()) {
            runCheck();
        }
    }
    return false;  // idle: the task sleeps on its queue between passes
}

void FirmwareReleases::runCheck() {
    const uint32_t startMs = millis();
    const CheckOutcome outcome = hubbo::release::checkForUpdate(feed_, running());
    state_.finishCheck(outcome, millis());
    const char *error = hubbo::release::checkErrorName(outcome.error);
    if (error != nullptr) {
        con.printf("update: check failed (%s) in %lu ms\n", error, static_cast<unsigned long>(millis() - startMs));
    } else if (outcome.available) {
        con.printf("update: %s is available\n", outcome.available->version.c_str());
    } else {
        con.printf("update: nothing to install (%s)\n", hubbo::release::availabilityName(outcome.availability));
    }
}

void FirmwareReleases::download(hubbo::release::Manifest manifest) {
    const uint32_t startMs = millis();
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(manifest.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        con.printf("update: no PSRAM for %u bytes\n", static_cast<unsigned>(manifest.size));
        firmware_.failDownload(hubbo::api::kDownloadFailed);
        return;
    }
    size_t length = 0;
    switch (hubbo::release::downloadImage(feed_, manifest, buffer, manifest.size, hash_, &length)) {
        case hubbo::release::DownloadResult::Ok:
            break;
        case hubbo::release::DownloadResult::Failed:
            heap_caps_free(buffer);
            firmware_.failDownload(hubbo::api::kDownloadFailed);
            return;
        case hubbo::release::DownloadResult::Mismatch:
            heap_caps_free(buffer);
            firmware_.failDownload(hubbo::api::kImageMismatch);
            return;
    }
    con.printf("update: downloaded %s, %u bytes in %lu ms\n", manifest.version.c_str(), static_cast<unsigned>(length),
               static_cast<unsigned long>(millis() - startMs));
    // httpd validates it as an upload and writes it; the buffer is its from here.
    if (!hooks_.postToHttpd([this, buffer, length] { firmware_.installDownloaded(buffer, length); })) {
        heap_caps_free(buffer);
        firmware_.failDownload(hubbo::api::kDownloadFailed);
    }
}

void FirmwareReleases::checkHealth(uint32_t nowMs) {
    if (!firmware_.trial()) {  // httpd marked it valid
        onTrial_ = false;
        return;
    }
    if (markPosted_) {
        return;  // healthy; waiting for httpd to mark it
    }
    const bool network = ethernet_.up();
    const char *webError = "no network";
    const bool web = network && webAnswers(&webError);
    const uint8_t health = (network ? 1 : 0) | (configLoaded_ ? 2 : 0) | (web ? 4 : 0);
    const bool expired = nowMs >= kTrialMs;
    if (health != lastHealth_ || expired) {
        lastHealth_ = health;
        con.printf("update: trial at %lu s: network %s, config %s, web %s\n", static_cast<unsigned long>(nowMs / 1000),
                   network ? "up" : "down", configLoaded_ ? "loaded" : "not loaded", web ? "answers" : webError);
    }
    if (network && configLoaded_ && web) {
        // Marking writes otadata (flash): httpd, not this PSRAM-stack task.
        markPosted_ = hooks_.postToHttpd([this, nowMs] {
            const esp_err_t marked = esp_ota_mark_app_valid_cancel_rollback();
            if (marked != ESP_OK) {
                con.printf("update: couldn't mark the image valid (%s)\n", esp_err_to_name(marked));
                markPosted_ = false;  // the next pass tries again
                return;
            }
            store_.remove(kPendingKey);
            firmware_.setTrial(false);
            con.printf("update: healthy after %lu s; the image is marked valid\n",
                       static_cast<unsigned long>(nowMs / 1000));
        });
    } else if (expired) {
        onTrial_ = false;
        con.println("update: not healthy in time; restarting into the previous image");
        hooks_.reboot("update trial failed");
    }
}

// Any HTTP answer from the hub's own web server counts. Over loopback: a
// connection to the hub's own Ethernet address doesn't connect.
bool FirmwareReleases::webAnswers(const char **why) const {
    esp_http_client_config_t config{};
    config.url = "http://127.0.0.1/api/v1/status";
    config.timeout_ms = kSelfRequestTimeoutMs;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    bool answered = false;
    const esp_err_t opened = esp_http_client_open(client, 0);
    if (opened == ESP_OK) {
        esp_http_client_fetch_headers(client);
        answered = esp_http_client_get_status_code(client) > 0;
        *why = "no status";
    } else {
        *why = esp_err_to_name(opened);
    }
    esp_http_client_cleanup(client);
    return answered;
}

hubbo::release::RunningFirmware FirmwareReleases::running() const {
    hubbo::release::RunningFirmware r;
    r.version = versionOverride_.empty() ? std::string(image::version()) : versionOverride_;
    r.board = board::kName;
    r.layout = board::kLayout;
    const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
    r.slotBytes = slot != nullptr ? slot->size : 0;
    return r;
}

}  // namespace hubbo::fw

// Arduino marks a new image valid before setup() unless this says to wait:
// the trial above marks it once healthy (docs/design/firmware-updates.md).
extern "C" bool verifyRollbackLater() {
    return true;
}
