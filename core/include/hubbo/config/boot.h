#pragma once

#include <cstdint>
#include <string>

#include "hubbo/auth/credentials.h"
#include "hubbo/config/service.h"
#include "hubbo/domain/config.h"
#include "hubbo/domain/error.h"
#include "hubbo/engine/active_persister.h"
#include "hubbo/engine/engine.h"

// One boot path (docs/design/persistence-and-contracts.md, section 6):
// load the config, then the credentials, then restore the active activity
// (without running its startup), following the persister rules. The
// firmware and hub-sim both call bootHub(); neither boots by hand.
//
// The active activity is restored only when the config loaded Ok from its
// record (not Fresh, Blocked or StorageUnavailable) and the saved value is
// readable. Otherwise the saved value means "unknown" (not "none"):
// nothing is active and the persister keeps what's saved until the user
// changes the active activity.
namespace hubbo {

enum class ActiveRestore : uint8_t {
    Restored,   // the saved activity is active (startup not run)
    NoneSaved,  // nothing (or "none") was saved: nothing active, known
    Unknown,    // couldn't restore: nothing active, saved value kept (see `reason`)
};
const char *activeRestoreName(ActiveRestore restore);  // "restored", "none_saved", "unknown"

struct BootReport {
    config::LoadReport config;
    // Unavailable if no Credentials were given.
    auth::CredentialState credentials = auth::CredentialState::Unavailable;
    ActiveRestore active = ActiveRestore::NoneSaved;
    // Unknown: why. StorageFailed / Corrupt (saved value unreadable),
    // ConfigBlocked (config didn't load Ok: Fresh, Blocked or
    // StorageUnavailable),
    // UnknownActivity (the config doesn't have it).
    ErrorCode reason = ErrorCode::Ok;
    std::string activityId;  // the saved id (restored, or kept while Unknown)
};

// Call once at boot, on the task that owns the engine before it runs (or
// with the engine idle). `credentials` may be null (no auth on this hub).
BootReport bootHub(const ConfigSpec &defaults, config::ConfigService &config, auth::Credentials *credentials,
                   Engine &engine, ActivePersister &persister, uint32_t nowMs);

}  // namespace hubbo
