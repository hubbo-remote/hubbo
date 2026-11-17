#include "firmware_update.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/efuse_hal.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "board.h"
#include "console.h"
#include "ota.h"

namespace hubbo::fw {

using hubbo::api::FirmwareInstallResult;
using hubbo::api::FirmwareUpdateState;

namespace {

constexpr size_t kChunkBytes = 4096;  // one flash sector: one erase per write
constexpr uint32_t kWriterStackBytes = 4096;
constexpr UBaseType_t kWriterPriority = 3;  // below HA (4) and httpd (5)
constexpr int kWriterCore = 0;

// What an image must match on this hub, writing to `slot`.
hubbo::api::FirmwareTarget targetFor(const esp_partition_t &slot) {
    hubbo::api::FirmwareTarget target;
    target.board = board::kName;
    target.layout = board::kLayout;
    target.chipRevision = static_cast<uint16_t>(efuse_hal_chip_revision());
    target.slotBytes = slot.size;
    return target;
}


}  // namespace

hubbo::api::FirmwareUpdateStatus FirmwareUpdate::status() const {
    hubbo::api::FirmwareUpdateStatus status;
    status.state = state_.load(std::memory_order_acquire);
    status.progress = progress_.load(std::memory_order_relaxed);
    if (status.state == FirmwareUpdateState::Failed) {
        status.error = error_.load(std::memory_order_relaxed);
    }
    return status;
}

bool FirmwareUpdate::claimForUpdate() {
    // On trial, the running image isn't confirmed yet (esp_ota_begin refuses).
    // Ours running: it holds the claim. Then the owner flag: ArduinoOTA
    // writing, or a reboot pending.
    return !trial() && !hubbo::api::firmwareUpdateRunning(state_.load()) && ota::claim(ota::Owner::Web);
}

FirmwareInstallResult FirmwareUpdate::check(std::string_view image, const esp_partition_t **slot,
                                            std::string *version) {
    *slot = esp_ota_get_next_update_partition(nullptr);
    if (*slot == nullptr) {
        return FirmwareInstallResult::Unavailable;
    }
    const uint32_t startUs = micros();
    const FirmwareInstallResult valid = hubbo::api::validateFirmwareImage(image, targetFor(**slot), hash_, version);
    con.printf("update: %s %s, %u bytes (result %u, checked in %lu ms)\n",
               valid == FirmwareInstallResult::Accepted ? "accepted" : "refused", version->c_str(),
               static_cast<unsigned>(image.size()), static_cast<unsigned>(valid),
               static_cast<unsigned long>((micros() - startUs) / 1000));
    return valid;
}

FirmwareInstallResult FirmwareUpdate::install(std::string_view image) {
    if (!claimForUpdate()) {
        return FirmwareInstallResult::Busy;
    }
    auto refuse = [](FirmwareInstallResult result) {
        ota::release(ota::Owner::Web);
        return result;
    };
    const esp_partition_t *slot = nullptr;
    std::string version;
    const FirmwareInstallResult valid = check(image, &slot, &version);
    if (valid != FirmwareInstallResult::Accepted) {
        return refuse(valid);
    }
    auto *copy = static_cast<uint8_t *>(heap_caps_malloc(image.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        con.printf("update: no PSRAM for a %u-byte copy\n", static_cast<unsigned>(image.size()));
        return refuse(FirmwareInstallResult::Unavailable);
    }
    memcpy(copy, image.data(), image.size());
    if (startWrite(copy, image.size(), slot, std::move(version)) != ESP_OK) {
        heap_caps_free(copy);
        return refuse(FirmwareInstallResult::Unavailable);
    }
    return FirmwareInstallResult::Accepted;
}

bool FirmwareUpdate::beginDownload() {
    if (!claimForUpdate()) {
        return false;
    }
    progress_.store(0, std::memory_order_relaxed);
    error_.store("", std::memory_order_relaxed);
    state_.store(FirmwareUpdateState::Downloading, std::memory_order_release);
    return true;
}

void FirmwareUpdate::failDownload(const char *error) {
    con.printf("update: release install failed (%s)\n", error);
    fail(error);
}

FirmwareInstallResult FirmwareUpdate::installDownloaded(uint8_t *image, size_t size) {
    auto refuse = [&](const char *error, FirmwareInstallResult result) {
        heap_caps_free(image);
        failDownload(error);
        return result;
    };
    const esp_partition_t *slot = nullptr;
    std::string version;
    const FirmwareInstallResult valid = check(std::string_view(reinterpret_cast<const char *>(image), size), &slot,
                                              &version);
    if (valid == FirmwareInstallResult::Unavailable) {
        return refuse("no_slot", valid);
    }
    if (valid != FirmwareInstallResult::Accepted) {
        return refuse(hubbo::api::kInvalidImage, valid);
    }
    if (startWrite(image, size, slot, std::move(version)) != ESP_OK) {
        return refuse("ota_unavailable", FirmwareInstallResult::Unavailable);
    }
    return FirmwareInstallResult::Accepted;
}

esp_err_t FirmwareUpdate::startWrite(uint8_t *image, size_t size, const esp_partition_t *slot, std::string version) {
    // Sequential writes: nothing is erased here; each write erases its sector.
    esp_ota_handle_t handle = 0;
    const esp_err_t begun = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (begun != ESP_OK) {
        con.printf("update: esp_ota_begin failed (%s)\n", esp_err_to_name(begun));
        return begun;
    }
    image_ = image;
    size_ = size;
    handle_ = handle;
    slot_ = slot;
    version_ = std::move(version);
    progress_.store(0, std::memory_order_relaxed);
    state_.store(FirmwareUpdateState::Writing, std::memory_order_release);
    // Internal RAM stack (the default): it runs with the cache off.
    if (xTaskCreatePinnedToCore(&writerTask, "fw_write", kWriterStackBytes, this, kWriterPriority, nullptr,
                                kWriterCore) != pdPASS) {
        esp_ota_abort(handle);
        image_ = nullptr;
        state_.store(FirmwareUpdateState::Idle, std::memory_order_release);
        con.println("update: couldn't start the writer task");
        return ESP_ERR_NO_MEM;
    }
    con.printf("update: writing %s to %s\n", version_.c_str(), slot->label);
    return ESP_OK;
}

void FirmwareUpdate::writerTask(void *self) {
    static_cast<FirmwareUpdate *>(self)->write();
    vTaskDelete(nullptr);
}

void FirmwareUpdate::write() {
    const uint32_t startMs = millis();
    // The flash driver can't read PSRAM with the cache off, so it would
    // bounce a PSRAM source through buffers of its own; one internal sector
    // buffer keeps it to one write per sector.
    auto *chunk = static_cast<uint8_t *>(heap_caps_malloc(kChunkBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (chunk == nullptr) {
        esp_ota_abort(handle_);
        fail("no_memory");
        return;
    }
    for (size_t done = 0; done < size_;) {
        const size_t n = std::min(kChunkBytes, size_ - done);
        memcpy(chunk, image_ + done, n);
        const esp_err_t err = esp_ota_write(handle_, chunk, n);
        if (err != ESP_OK) {
            heap_caps_free(chunk);
            esp_ota_abort(handle_);
            con.printf("update: write failed at %u (%s)\n", static_cast<unsigned>(done), esp_err_to_name(err));
            fail("write_failed");
            return;
        }
        done += n;
        progress_.store(static_cast<uint8_t>(uint64_t{done} * 100 / size_), std::memory_order_relaxed);
        vTaskDelay(1);  // between sectors: httpd, IDLE0 (the watchdog) and the network run
    }
    heap_caps_free(chunk);
    heap_caps_free(image_);
    image_ = nullptr;

    // Reads the slot back and checks the image, its SHA-256 included.
    const esp_err_t ended = esp_ota_end(handle_);
    if (ended != ESP_OK) {
        con.printf("update: the written image failed its check (%s)\n", esp_err_to_name(ended));
        fail("verify_failed");
        return;
    }
    const esp_err_t set = esp_ota_set_boot_partition(static_cast<const esp_partition_t *>(slot_));
    if (set != ESP_OK) {
        con.printf("update: couldn't set the boot slot (%s)\n", esp_err_to_name(set));
        fail("boot_select_failed");
        return;
    }
    con.printf("update: written in %lu ms, stack %lu bytes unused; rebooting\n",
               static_cast<unsigned long>(millis() - startMs),
               static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
    state_.store(FirmwareUpdateState::Rebooting, std::memory_order_release);
    if (written_) {
        written_(version_);  // the trial's record, before the reboot
    }
    reboot_("firmware update");  // the owner claim stays: no other update before the restart
}

// The writer task. The running image stays the boot image.
void FirmwareUpdate::fail(const char *error) {
    heap_caps_free(image_);
    image_ = nullptr;
    error_.store(error, std::memory_order_relaxed);
    // Release before Failed shows: claimForUpdate() checks the state first, so
    // until then a new update is Busy and never shares this claim.
    ota::release(ota::Owner::Web);
    state_.store(FirmwareUpdateState::Failed, std::memory_order_release);
}

}  // namespace hubbo::fw
