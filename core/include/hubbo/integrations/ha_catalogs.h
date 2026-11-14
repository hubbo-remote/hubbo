#pragma once

#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include "hubbo/domain/adapter.h"
#include "hubbo/domain/config.h"
#include "hubbo/ha/capabilities.h"

// The Home Assistant command catalogs a home_assistant adapter hands out,
// and the service calls built from them. Shared by the real adapter and
// hub-sim's scripted fake, so both compile and build calls the same way.
//
// Nothing is ever freed: a catalog's CommandSpecs are pointed at from more
// than compiled configs (queued and in-flight executor work, held keys, an
// EngineRequest's CompiledCommand, the adapter's hand-over ring), so they
// live until the adapter does (a reboot). Growth is one set per capabilities
// change (a device re-registered after HA changed it), not per compile.
//
// Thread-safe: the config task adds, the integration task (or the fake's
// engine task) looks up.
namespace hubbo::integrations {

class HaCatalogs {
public:
    // The device's commands from its stored capabilities snapshot, cached
    // per device; a changed snapshot gets new storage. Empty without a
    // snapshot (the device can't be targeted).
    CommandCatalog commandsFor(const DeviceSpec &device);
    // Keeps `capabilities` (built some other way) for the adapter's life.
    const ha::EntityCapabilities &keep(ha::EntityCapabilities capabilities);

    // The kept capabilities `command` belongs to, if they are `entityId`'s;
    // else nullptr. Resolve once per call: kept capabilities never change or
    // move, so the result is used without the lock.
    const ha::EntityCapabilities *owner(const CommandSpec *command, std::string_view entityId) const;

    // The HA service call for the command at `index` of `capabilities`: a
    // Home Assistant action (`custom`, call_service) as the user wrote it,
    // else the catalog's command with its params and the `state` read for
    // it (stateAttribute()).
    static ErrorCode buildCall(const ha::EntityCapabilities &capabilities, uint16_t index, const ParamValue *params,
                               uint8_t paramsPresent, const HaServiceCallSpec *custom, const ha::ServiceValue *state,
                               ha::ServiceCall *out);

private:
    struct Cached {
        // What the capabilities were built from.
        std::string entityId;
        ha::CapabilitySnapshot snapshot;
        const ha::EntityCapabilities *capabilities = nullptr;
    };
    mutable std::mutex mutex_;
    std::map<std::string, Cached, std::less<>> byDevice_;
    std::deque<ha::EntityCapabilities> storage_;  // stable addresses, never shrinks
};

}  // namespace hubbo::integrations
