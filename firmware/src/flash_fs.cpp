#include "flash_fs.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_partition.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <new>

namespace hubbo::fw::flash_fs {

namespace {

constexpr const char *kPartition = "spiffs";
constexpr uint8_t kMaxOpenFiles = 4;
constexpr const char *kPrefsNamespace = "hubbo-fs";
constexpr const char *kPrefsFormatRequested = "format_req";

bool isMounted = false;

enum class Flag { Set, Clear, Unknown };

// "Unknown" (namespace or key unreadable) is not "requested": nothing is
// formatted on it.
Flag readFormatRequested() {
    Preferences prefs;
    if (!prefs.begin(kPrefsNamespace, true)) {
        return Flag::Clear;  // namespace never created: nothing was ever requested
    }
    Flag flag = Flag::Unknown;
    if (!prefs.isKey(kPrefsFormatRequested)) {
        flag = Flag::Clear;
    } else {
        // getBool can't tell "false" from "unreadable", so read the raw byte.
        const uint8_t sentinel = 0xA5;
        const uint8_t value = prefs.getUChar(kPrefsFormatRequested, sentinel);
        flag = value == sentinel ? Flag::Unknown : (value != 0 ? Flag::Set : Flag::Clear);
    }
    prefs.end();
    return flag;
}

bool setFormatRequested(bool requested) {
    Preferences prefs;
    if (!prefs.begin(kPrefsNamespace, false)) {
        return false;
    }
    const bool ok = prefs.putUChar(kPrefsFormatRequested, requested ? 1 : 0) > 0;
    prefs.end();
    return ok;
}

// True only if every byte of the partition reads back as 0xFF. A read error
// is "don't know", so false.
bool partitionErased() {
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPartition);
    if (partition == nullptr) {
        return false;
    }
    constexpr size_t kChunkWords = 1024;  // 4 KB, one flash sector; boot only, so heap
    std::unique_ptr<uint32_t[]> chunk(new (std::nothrow) uint32_t[kChunkWords]);
    if (!chunk) {
        return false;
    }
    constexpr size_t kChunkBytes = kChunkWords * sizeof(uint32_t);
    for (size_t offset = 0; offset < partition->size; offset += kChunkBytes) {
        const size_t length = std::min(kChunkBytes, static_cast<size_t>(partition->size - offset));
        if (esp_partition_read(partition, offset, chunk.get(), length) != ESP_OK) {
            return false;
        }
        for (size_t i = 0; i < length / sizeof(uint32_t); i++) {
            if (chunk[i] != 0xFFFFFFFFu) {
                return false;
            }
        }
    }
    return true;
}

bool begin() {
    isMounted = LittleFS.begin(false, kMountPoint, kMaxOpenFiles, kPartition);
    return isMounted;
}

// Boot only, before anything uses the filesystem. begin() has run first, so
// LittleFS knows the partition label even if mounting failed.
bool formatAndMount() {
    if (isMounted) {
        LittleFS.end();
        isMounted = false;
    }
    return LittleFS.format() && begin();
}

}  // namespace

MountResult mount() {
    const bool mountedNow = begin();
    if (readFormatRequested() == Flag::Set) {
        // A destructive action needs its trigger cleared first: if the flag
        // can't be durably cleared, formatting now would repeat on every boot.
        if (!setFormatRequested(false) || readFormatRequested() != Flag::Clear) {
            return mountedNow ? MountResult::FormatRefusedMounted : MountResult::FormatRefused;
        }
        return formatAndMount() ? MountResult::FormattedOnRequest : MountResult::Failed;
    }
    if (mountedNow) {
        return MountResult::Mounted;
    }
    if (partitionErased()) {
        return formatAndMount() ? MountResult::FormattedBlank : MountResult::Failed;
    }
    return MountResult::Failed;
}

bool mounted() {
    return isMounted;
}

bool requestFormatAtBoot() {
    return setFormatRequested(true);
}

size_t totalBytes() {
    return isMounted ? LittleFS.totalBytes() : 0;
}

size_t usedBytes() {
    return isMounted ? LittleFS.usedBytes() : 0;
}

}  // namespace hubbo::fw::flash_fs
