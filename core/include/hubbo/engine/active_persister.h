#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "hubbo/domain/error.h"
#include "hubbo/storage/state_store.h"

// Remembers the active activity across reboots without writing flash on the
// hot path. docs/design/persistence-and-contracts.md, sections 1 and 5.
//
// The config/HTTP side calls tick() regularly with the engine's current
// active id (Engine::activeActivityIdShared(), safe from any task), the
// injected clock and whether the config is Ok. A change is written once it
// has been stable for `delayMs` (default 5 s) and only if it differs from
// what is saved, so switching twice inside the delay costs one write and
// switching back costs none. "" (nothing active / Off) is saved as "". A
// failed write is retried after another delay.
//
// Secondary state follows primary state:
//   - nothing is written while the config isn't Ok (Blocked, storage
//     unavailable): the active activity is derived from it;
//   - after a boot that couldn't restore the saved activity (config not Ok,
//     or the activity unknown), the current "" means "unknown", not "none":
//     holdUntilChanged() keeps the saved value until the active activity
//     actually changes (a user activation or Off).
//   - an unreadable saved value is unknown too; it's never taken as "none".
namespace hubbo {

class ActivePersister {
public:
    static constexpr uint32_t kDefaultDelayMs = 5000;
    static constexpr const char *kKey = "active_activity";

    explicit ActivePersister(storage::StateStore &store, uint32_t delayMs = kDefaultDelayMs);

    // Boot: the saved id. Ok ("" = none saved, or saved as none), NotFound
    // (never saved), or anything else (StorageFailed, Corrupt: unreadable,
    // unknown). Call once, before tick(). bootHub() (config/boot.h) is the
    // one caller.
    ErrorCode savedActivity(std::string *out);

    // Keep what's saved while the active id stays `current` (see above).
    void holdUntilChanged(std::string_view current);

    // `current`: the engine's active id ("" = none). `configOk`: the config
    // is Ok (ConfigService::state() == ConfigState::Ok).
    void tick(std::string_view current, uint32_t nowMs, bool configOk);

    // Graceful shutdown (hub-sim SIGTERM, firmware OTA reboot): writes
    // `current` now if a write is due by the rules above. A power cut loses
    // at most the delay.
    void flush(std::string_view current, bool configOk);

    bool pending() const;
    uint32_t writes() const { return writes_; }

private:
    void observe(std::string_view current, uint32_t nowMs);
    bool write();

    storage::StateStore &store_;
    uint32_t delayMs_;
    std::string saved_;     // what the store holds (when known)
    bool savedKnown_ = false;
    std::string observed_;  // last value seen
    bool started_ = false;
    bool holding_ = false;  // keep saved_ until observed_ changes
    uint32_t changedAtMs_ = 0;
    uint32_t writes_ = 0;
};

}  // namespace hubbo
