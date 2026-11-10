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

FirmwareInstallResult FirmwareUpdate::install(std::string_view image) {
    if (!ota::claim(ota::Owner::Web)) {
        return FirmwareInstallResult::Busy;  // ArduinoOTA is writing, or a reboot is pending
    }
    const FirmwareUpdateState state = state_.load();
    if (state == FirmwareUpdateState::Writing || state == FirmwareUpdateState::Rebooting) {
        return FirmwareInstallResult::Busy;  // ours: the claim stays with the running update
    }
    auto refuse = [](FirmwareInstallResult result) {
        ota::release(ota::Owner::Web);
        return result;
    };

    const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
    if (slot == nullptr) {
        return refuse(FirmwareInstallResult::Unavailable);
    }
    std::string version;
    const uint32_t startUs = micros();
    const FirmwareInstallResult valid = hubbo::api::validateFirmwareImage(image, targetFor(*slot), hash_, &version);
    const uint32_t checkMs = (micros() - startUs) / 1000;
    if (valid != FirmwareInstallResult::Accepted) {
        con.printf("update: refused %u bytes (result %u, %lu ms)\n", static_cast<unsigned>(image.size()),
                   static_cast<unsigned>(valid), static_cast<unsigned long>(checkMs));
        return refuse(valid);
    }

    auto *copy = static_cast<uint8_t *>(heap_caps_malloc(image.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        con.printf("update: no PSRAM for a %u-byte copy\n", static_cast<unsigned>(image.size()));
        return refuse(FirmwareInstallResult::Unavailable);
    }
    memcpy(copy, image.data(), image.size());
    // Sequential writes: nothing is erased here; each write erases its sector.
    esp_ota_handle_t handle = 0;
    const esp_err_t begun = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (begun != ESP_OK) {
        heap_caps_free(copy);
        con.printf("update: esp_ota_begin failed (%s)\n", esp_err_to_name(begun));
        return refuse(FirmwareInstallResult::Unavailable);
    }
    image_ = copy;
    size_ = image.size();
    handle_ = handle;
    slot_ = slot;
    progress_.store(0, std::memory_order_relaxed);
    state_.store(FirmwareUpdateState::Writing, std::memory_order_release);
    // Internal RAM stack (the default): it runs with the cache off.
    if (xTaskCreatePinnedToCore(&writerTask, "fw_write", kWriterStackBytes, this, kWriterPriority, nullptr,
                                kWriterCore) != pdPASS) {
        esp_ota_abort(handle);
        heap_caps_free(copy);
        image_ = nullptr;
        state_.store(FirmwareUpdateState::Idle, std::memory_order_release);
        con.println("update: couldn't start the writer task");
        return refuse(FirmwareInstallResult::Unavailable);
    }
    con.printf("update: accepted %s, %u bytes (checked in %lu ms); writing to %s\n", version.c_str(),
               static_cast<unsigned>(size_), static_cast<unsigned long>(checkMs), slot->label);
    return FirmwareInstallResult::Accepted;
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
    reboot_("firmware update");  // the owner claim stays: no other update before the restart
}

// The writer task. The running image stays the boot image.
void FirmwareUpdate::fail(const char *error) {
    heap_caps_free(image_);
    image_ = nullptr;
    error_.store(error, std::memory_order_relaxed);
    state_.store(FirmwareUpdateState::Failed, std::memory_order_release);
    ota::release(ota::Owner::Web);
}

}  // namespace hubbo::fw
