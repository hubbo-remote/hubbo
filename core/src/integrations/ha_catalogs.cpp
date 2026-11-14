#include "hubbo/integrations/ha_catalogs.h"

#include <utility>

namespace hubbo::integrations {

CommandCatalog HaCatalogs::commandsFor(const DeviceSpec &device) {
    if (!device.haCapabilities) return {};
    const std::string entityId = providerRef(device);
    const ha::CapabilitySnapshot &snapshot = *device.haCapabilities;

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = byDevice_.find(device.id);
    if (it != byDevice_.end() && it->second.entityId == entityId && it->second.snapshot == snapshot) {
        return it->second.capabilities->catalog();
    }
    // New or changed: new storage; the old catalog stays valid for whatever
    // still points at it.
    storage_.push_back(ha::EntityCapabilities::fromSnapshot(entityId, snapshot));
    byDevice_[device.id] = Cached{entityId, snapshot, &storage_.back()};
    return storage_.back().catalog();
}

const ha::EntityCapabilities &HaCatalogs::keep(ha::EntityCapabilities capabilities) {
    std::lock_guard<std::mutex> lock(mutex_);
    storage_.push_back(std::move(capabilities));
    return storage_.back();
}

const ha::EntityCapabilities *HaCatalogs::owner(const CommandSpec *command, std::string_view entityId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const ha::EntityCapabilities &caps : storage_) {
        const CommandCatalog catalog = caps.catalog();
        if (catalog.count > 0 && command >= catalog.items && command < catalog.items + catalog.count) {
            return caps.entityId() == entityId ? &caps : nullptr;
        }
    }
    return nullptr;
}

ErrorCode HaCatalogs::buildCall(const ha::EntityCapabilities &capabilities, uint16_t index, const ParamValue *params,
                                uint8_t paramsPresent, const HaServiceCallSpec *custom, const ha::ServiceValue *state,
                                ha::ServiceCall *out) {
    return custom != nullptr ? capabilities.buildCustomCall(custom->service, custom->data, out)
                             : capabilities.buildServiceCall(index, params, paramsPresent, out, state);
}

}  // namespace hubbo::integrations
