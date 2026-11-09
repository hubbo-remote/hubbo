#include "hubbo/config/boot.h"

namespace hubbo {

const char *activeRestoreName(ActiveRestore restore) {
    switch (restore) {
        case ActiveRestore::Restored: return "restored";
        case ActiveRestore::NoneSaved: return "none_saved";
        case ActiveRestore::Unknown: return "unknown";
    }
    return "unknown";
}

BootReport bootHub(const ConfigSpec &defaults, config::ConfigService &config, auth::Credentials *credentials,
                   Engine &engine, ActivePersister &persister, uint32_t nowMs) {
    BootReport report;
    report.config = config.load(defaults);
    if (credentials != nullptr) {
        credentials->load();
        report.credentials = credentials->state();
    }

    const ErrorCode read = persister.savedActivity(&report.activityId);
    auto unknown = [&](ErrorCode reason) {
        report.active = ActiveRestore::Unknown;
        report.reason = reason;
        persister.holdUntilChanged(engine.activeActivityIdShared());
        return report;
    };
    if (read != ErrorCode::Ok && read != ErrorCode::NotFound) return unknown(read);  // unreadable: unknown
    if (report.activityId.empty()) {  // readable and none: known, whatever the config
        report.active = ActiveRestore::NoneSaved;
        return report;
    }
    // Secondary state follows primary state: restore only into an Ok config
    // loaded from its record. Fresh (the defaults, even once saved) isn't the
    // config the saved id came from.
    if (report.config.state != config::ConfigState::Ok || config.state() != config::ConfigState::Ok) {
        return unknown(ErrorCode::ConfigBlocked);
    }
    const ErrorCode restored = engine.restoreActive(report.activityId, nowMs);
    if (restored != ErrorCode::Ok) return unknown(restored);
    report.active = ActiveRestore::Restored;
    return report;
}

}  // namespace hubbo
