#pragma once

// Online firmware updates on the board (docs/design/firmware-updates.md):
// api::FirmwareReleasePort, and the trial every new image boots on.
//
// Tasks: the port's methods run on httpd and return at once. The "update"
// task (an IntegrationTask: core 0, priority 4, PSRAM stack) runs checks,
// downloads and the trial's health checks; it blocks on the network and never
// touches flash, so store writes and marking the image valid are posted to
// httpd. The RF loop never waits on it.
//
// The trial: verifyRollbackLater() is overridden, so a new image boots
// PENDING_VERIFY. Healthy within kTrialMs (Ethernet up, config loaded, the web
// server answers 127.0.0.1): httpd marks it valid. Otherwise the hub restarts
// and the bootloader goes back. Meanwhile every update is Busy.
//
// rolled_back_from: the writer saves the version it wrote (kPendingKey). At
// boot, a saved version that isn't the running one means the bootloader went
// back; it moves to kRolledBackKey until the next update starts.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "esp_release_feed.h"
#include "firmware_update.h"
#include "hubbo/api/firmware_release_port.h"
#include "hubbo/auth/crypto.h"
#include "hubbo/config/hub.h"
#include "hubbo/storage/state_store.h"
#include "integration_task.h"

namespace hubbo::fw {

class EthernetLink;

class FirmwareReleases : public hubbo::api::FirmwareReleasePort {
public:
    static constexpr uint32_t kFirstCheckMs = 60 * 1000;
    static constexpr uint32_t kCheckEveryMs = 24 * 60 * 60 * 1000;
    static constexpr uint32_t kTrialMs = 2 * 60 * 1000;
    static constexpr uint32_t kHealthEveryMs = 1000;
    static constexpr const char *kPendingKey = "fw_pending";        // StateStore: the version just written
    static constexpr const char *kRolledBackKey = "fw_rolled_back";  // the version the bootloader left

    struct Hooks {
        std::function<bool(std::function<void()>)> postToHttpd;  // HttpServer::post
        void (*reboot)(const char *why);                          // requestReboot
    };

    // Registers its hooks on `firmware` (written, download progress). `hash`
    // is stateless (EspHash), so the update task shares it.
    FirmwareReleases(FirmwareUpdate &firmware, hubbo::storage::StateStore &store,
                     const hubbo::config::HubSettings &settings, const EthernetLink &ethernet, hubbo::auth::Hash &hash,
                     Hooks hooks);

    // setup(), once the store is mounted and httpd runs (it posts the
    // record's updates): the trial and the rollback record. `configLoaded`:
    // the boot config is Ok, Blocked or Fresh.
    void boot(bool configLoaded);
    // setup(), after boot(): the update task.
    bool start();

    // api::FirmwareReleasePort (httpd).
    hubbo::api::FirmwareReleaseStatus releaseStatus() const override;
    hubbo::api::ReleaseCheckResult requestCheck() override;
    hubbo::api::ReleaseInstallResult install(std::string_view version) override;

#ifdef HUBBO_DEV_CONSOLE
    // The dev console's /debug/release: a local feed, and the version checks
    // compare against (a dev build is never offered anything). On the update
    // task, so no fetch is running.
    bool setTest(std::string baseUrl, std::string runningVersion);
#endif

private:
    void onWritten(const std::string &version);  // the writer task (FirmwareUpdate::setOnWritten)
    bool service();  // the update task's pass
    void runCheck();
    void download(hubbo::release::Manifest manifest);
    void checkHealth(uint32_t nowMs);
    bool webAnswers(const char **why) const;  // why: a literal, when false
    hubbo::release::RunningFirmware running() const;

    FirmwareUpdate &firmware_;
    hubbo::storage::StateStore &store_;
    const hubbo::config::HubSettings &settings_;
    const EthernetLink &ethernet_;
    Hooks hooks_;

    hubbo::release::UpdateCheckState state_;
    IntegrationTask task_;
    // The update task's own.
    EspReleaseFeed feed_;
    hubbo::auth::Hash &hash_;
#ifdef HUBBO_DEV_CONSOLE
    std::string versionOverride_;  // setTest()
#endif
    uint32_t nextCheckMs_ = kFirstCheckMs;
    uint32_t nextHealthMs_ = 0;
    bool configLoaded_ = false;
    bool onTrial_ = false;                 // the task's: health checks run
    uint8_t lastHealth_ = 0xff;            // the task's: what the trial last logged
    std::atomic<bool> markPosted_{false};  // healthy, marking queued on httpd (cleared if marking fails)
};

}  // namespace hubbo::fw
