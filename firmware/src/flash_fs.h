#pragma once

// LittleFS on the `spiffs` data partition of default_16MB.csv (0xC90000,
// 3.4 MB), mounted at /fs. Holds the record store (config, active activity).
//
// Never destroys on uncertain information (docs/design/persistence-and-
// contracts.md, part 5). It formats only:
//   - when the partition positively reads as erased flash (every byte 0xFF:
//     a factory-fresh board), or
//   - at boot after an explicit request (`fs format` -> requestFormatAtBoot()
//     -> reboot), before anything uses the filesystem, and only once the
//     request flag has been durably cleared (re-read to confirm).
// Any other mount failure leaves the filesystem unmounted: the record store
// reports Unavailable and nothing is erased.

#include <cstddef>

namespace hubbo::fw::flash_fs {

constexpr const char *kMountPoint = "/fs";

enum class MountResult {
    Mounted,
    FormattedBlank,      // the partition was erased flash
    FormattedOnRequest,  // `fs format`
    // `fs format` was requested, but the request flag couldn't be durably
    // cleared, so formatting would repeat every boot: not formatted (the
    // filesystem is left as it was, mounted or not).
    FormatRefused,
    Failed,              // not mounted; nothing erased
};

// Call once at boot, before anything uses the filesystem.
MountResult mount();
bool mounted();

// Erase the partition at the next boot (before anything else runs). Writes
// NVS: call it from the httpd task, never the RF task.
bool requestFormatAtBoot();

size_t totalBytes();
size_t usedBytes();

}  // namespace hubbo::fw::flash_fs
