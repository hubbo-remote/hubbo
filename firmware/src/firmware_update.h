#pragma once

// api::FirmwareUpdatePort over esp_ota: updates from the web admin
// (docs/design/phase5-appliance.md, section 2), uploads and release installs
// (docs/design/firmware-updates.md: FirmwareReleases downloads, then hands the
// buffer over with installDownloaded()). A new image boots on trial
// (firmware_releases.h); while a trial runs, every update is Busy.
//
// install() runs on httpd: it claims the OTA owner flag (ota.h; ArduinoOTA
// is the other writer; refused while a reboot is pending), checks the whole image with
// api::validateFirmwareImage() before a byte is erased, copies it to PSRAM
// (the transport frees its buffer after the response), opens the inactive
// slot and starts a one-shot writer task.
//
// The writer task (core 0, internal-RAM stack: flash writes turn the cache
// off, so a PSRAM stack can't be used) writes 4 KB at a time with
// OTA_WITH_SEQUENTIAL_WRITES, so one sector is erased per write, and yields
// a tick after each. Every erase and write still stalls both cores, the RF
// loop included, for the length of the flash operation: the remote may lag
// for the ~30 s of a write (accepted). esp_ota_end() checks the image again
// from flash; then the boot slot is set and the hub restarts through the
// caller's reboot path (requestReboot), which releases keys and flushes the
// active activity. The task never waits on another one.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <esp_err.h>
#include <esp_partition.h>

#include "hubbo/api/firmware_update_port.h"

namespace hubbo::fw {

class FirmwareUpdate : public hubbo::api::FirmwareUpdatePort {
public:
    // `reboot` requests the restart (any task; a string literal). `hash`
    // checks the image (httpd only).
    FirmwareUpdate(void (*reboot)(const char *why), hubbo::auth::Hash &hash) : reboot_(reboot), hash_(hash) {}

    // httpd.
    hubbo::api::FirmwareUpdateStatus status() const override;
    hubbo::api::FirmwareInstallResult install(std::string_view image) override;

    // A release install (FirmwareReleases). beginDownload() (httpd): false
    // if an update, a reboot or a trial is running; else the state is
    // Downloading and the owner flag is held. Then setDownloadProgress() (any
    // task) and either failDownload() (any task; `error` a string literal:
    // Failed, flag released) or installDownloaded() (httpd).
    bool beginDownload();
    void setDownloadProgress(uint8_t percent) { progress_.store(percent, std::memory_order_relaxed); }
    void failDownload(const char *error);
    // Takes `image` (heap_caps_malloc'd, PSRAM): validates it as an upload
    // and writes it with no copy, or frees it and fails with
    // kInvalidImage (or Unavailable's codes).
    hubbo::api::FirmwareInstallResult installDownloaded(uint8_t *image, size_t size);

    // While true (a new image on trial), install() and beginDownload() are
    // Busy. Any task.
    void setTrial(bool on) { trial_.store(on, std::memory_order_release); }
    bool trial() const { return trial_.load(std::memory_order_acquire); }

    // Once, before the API starts: runs on the writer task once an image is
    // written and set to boot, before the reboot request, with the image's
    // marker version.
    void setOnWritten(std::function<void(const std::string &version)> written) { written_ = std::move(written); }

private:
    static void writerTask(void *self);
    // install() and beginDownload(): false if a trial, another writer, a
    // pending reboot or our own update is running.
    bool claimForUpdate();
    // httpd: the inactive slot and validateFirmwareImage() against it, logged.
    // Unavailable when there's no slot.
    hubbo::api::FirmwareInstallResult check(std::string_view image, const esp_partition_t **slot,
                                            std::string *version);
    // httpd, the owner flag held: esp_ota_begin and the writer task, which
    // owns `image` from then on. Not ESP_OK: nothing started, `image` not freed.
    esp_err_t startWrite(uint8_t *image, size_t size, const esp_partition_t *slot, std::string version);
    void write();
    void fail(const char *error);

    void (*reboot_)(const char *);
    hubbo::auth::Hash &hash_;
    std::function<void(const std::string &)> written_;
    std::atomic<bool> trial_{false};

    // Owned by the writer task while Writing.
    uint8_t *image_ = nullptr;  // PSRAM copy
    size_t size_ = 0;
    uint32_t handle_ = 0;       // esp_ota_handle_t
    const void *slot_ = nullptr;  // const esp_partition_t *
    std::string version_;         // the image marker's

    std::atomic<hubbo::api::FirmwareUpdateState> state_{hubbo::api::FirmwareUpdateState::Idle};
    std::atomic<uint8_t> progress_{0};
    std::atomic<const char *> error_{""};  // string literals only
};

}  // namespace hubbo::fw
