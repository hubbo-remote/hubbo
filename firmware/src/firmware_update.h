#pragma once

// api::FirmwareUpdatePort over esp_ota: updates from the web admin
// (docs/design/phase5-appliance.md, section 2). No rollback: Arduino marks
// the running image valid before setup(), and that stays.
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

private:
    static void writerTask(void *self);
    void write();
    void fail(const char *error);

    void (*reboot_)(const char *);
    hubbo::auth::Hash &hash_;

    // Owned by the writer task while Writing.
    uint8_t *image_ = nullptr;  // PSRAM copy
    size_t size_ = 0;
    uint32_t handle_ = 0;       // esp_ota_handle_t
    const void *slot_ = nullptr;  // const esp_partition_t *

    std::atomic<hubbo::api::FirmwareUpdateState> state_{hubbo::api::FirmwareUpdateState::Idle};
    std::atomic<uint8_t> progress_{0};
    std::atomic<const char *> error_{""};  // string literals only
};

}  // namespace hubbo::fw
