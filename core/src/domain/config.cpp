#include "hubbo/domain/config.h"

#include "hubbo/domain/ids.h"

namespace hubbo {

std::string providerRef(const DeviceSpec &device) {
    if (device.provider.type == kHomeAssistantType) return device.provider.entityId;
    if (device.provider.type == kBleHidType) return device.id;  // its own host, bonded by device id
    return device.provider.entityId;
}

void ensureBuiltinIntegrations(ConfigSpec *config) {
    auto has = [&](const char *id) { return indexOfId(config->integrations, id) >= 0; };
    if (!has(kSystemIntegrationId)) {
        IntegrationSpec system;
        system.id = kSystemIntegrationId;
        system.type = kSystemType;
        system.name = "System";
        config->integrations.insert(config->integrations.begin(), system);
    }
    if (!has(kBleIntegrationId)) {
        IntegrationSpec ble;
        ble.id = kBleIntegrationId;
        ble.type = kBleHidType;
        ble.name = "Bluetooth";
        ble.deviceName = "Hubbo";
        config->integrations.insert(config->integrations.begin() + 1, ble);
    }
    if (!has(kHomeAssistantIntegrationId)) {
        IntegrationSpec ha;
        ha.id = kHomeAssistantIntegrationId;
        ha.type = kHomeAssistantType;
        ha.name = "Home Assistant";
        config->integrations.insert(config->integrations.begin() + 2, ha);
    }
}

}  // namespace hubbo
