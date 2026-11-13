#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "hubbo/domain/error.h"
#include "hubbo/storage/state_store.h"

// The hub's own name (GET/PUT /admin/hub, /status hubbo.name). It is not
// part of the activity config, so config saves, resets and backups never
// touch it: it lives in the StateStore under kHubNameKey (the board and
// hub-sim: a RecordStateStore record next to the config's, on LittleFS or
// the sim's storage).
//
// The name is shown as typed: the Bluetooth device name and the Bonjour
// service instance name. The .local hostname (and the DHCP hostname, at the
// next boot) is hostnameFor(name).
namespace hubbo::config {

constexpr const char *kDefaultHubName = "Hubbo";
constexpr const char *kDefaultHostname = "hubbo";
constexpr const char *kHubNameKey = "hub_name";  // StateStore key
constexpr size_t kHubNameMaxChars = 32;    // Unicode code points
constexpr size_t kHubNameMaxBytes = 128;   // their UTF-8 upper bound (4 bytes each)
constexpr size_t kHostnameMaxLength = 63;  // one DNS label
static_assert(kHubNameMaxBytes <= storage::kMaxStateValueLength, "the name fits the StateStore");

// The name as stored: `requested` with leading and trailing spaces removed.
std::string trimHubName(std::string_view requested);

// Checks an already trimmed name: Ok, Required (empty), TooLong (over
// kHubNameMaxChars code points) or InvalidName
// (not UTF-8, a control character, or a leading/trailing space).
ErrorCode checkHubName(std::string_view name);

// The DNS-safe .local hostname for a name: ASCII letters and digits,
// lowercased; every other run of characters becomes one '-'; no leading or
// trailing '-'; at most kHostnameMaxLength characters; "hubbo" if nothing is
// left. "Living Room" -> "living-room", "Hubbo" -> "hubbo".
std::string hostnameFor(std::string_view name);

struct HubIdentity {
    std::string name = kDefaultHubName;
    std::string hostname = kDefaultHostname;
};

// Owns the name: loads it at boot, renames it, and tells the platform.
// Any task may call it (a mutex guards the name); never the RF/engine task,
// since rename() writes flash.
class HubSettings {
public:
    // Called after a rename is saved, on the renaming task (HTTP), without
    // the lock held. The board applies it live to Bluetooth and mDNS.
    using Changed = std::function<void(const HubIdentity &identity)>;

    explicit HubSettings(storage::StateStore &store) : store_(store) {}

    // Boot (and after a wipe): the saved name, or the default. Returns the read's result: Ok,
    // NotFound (none saved: the default, known), or StorageFailed / Corrupt
    // (unreadable: the default for now, and nothing is written over what's
    // saved). A saved value that fails checkHubName() is Corrupt.
    ErrorCode load();

    HubIdentity identity() const;

    // Trims, checks and saves the name, then calls `changed`. The same name
    // as now is Ok without a save or a call. Returns Ok (`*out` is the new
    // identity, if given), a checkHubName() error
    // (nothing changes), or StorageFailed (the save failed: the old name
    // stays).
    ErrorCode rename(std::string_view requested, HubIdentity *out = nullptr);

    // A reset's or wipe's (hub-sim): removes the saved name and goes back
    // to the default, without calling `changed`. Ok or StorageFailed.
    ErrorCode forget();

    void onChanged(Changed changed);  // set once, before the API serves (unlocked)

private:
    storage::StateStore &store_;
    std::mutex renameMutex_;      // one rename (save + notify) at a time
    mutable std::mutex mutex_;    // identity_
    HubIdentity identity_;
    bool matchesStore_ = false;  // identity_ is what's saved (or none is): a same-name rename can skip the save
    Changed changed_;
};

}  // namespace hubbo::config
