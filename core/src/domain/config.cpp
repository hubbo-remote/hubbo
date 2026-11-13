#include "hubbo/domain/config.h"

#include "hubbo/domain/ids.h"

#include <cstddef>
#include <iterator>

namespace hubbo {

std::string providerRef(const DeviceSpec &device) {
    if (device.provider.type == kHomeAssistantType) return device.provider.entityId;
    if (device.provider.type == kBleHidType) return device.id;  // its own host, bonded by device id
    return device.provider.entityId;
}

const ProfileCommand *DeviceProfile::findCommand(std::string_view name) const {
    for (const ProfileCommand &c : commands) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

namespace {

// The built-ins, in the order they lead config.integrations.
struct Builtin {
    const char *id;
    const char *type;
    const char *name;
    const char *deviceName;
    bool enabled;
};
constexpr Builtin kBuiltins[] = {
    {kSystemIntegrationId, kSystemType, "System", "", true},
    {kBleIntegrationId, kBleHidType, "Bluetooth", "Hubbo", true},
    {kHomeAssistantIntegrationId, kHomeAssistantType, "Home Assistant", "", true},
    {kMqttIntegrationId, kMqttType, "MQTT", "", false},
};

}  // namespace

size_t integrationCountWithBuiltins(const std::vector<IntegrationSpec> &integrations) {
    size_t count = integrations.size();
    for (const Builtin &b : kBuiltins) count += indexOfId(integrations, b.id) < 0;
    return count;
}

void ensureBuiltinIntegrations(ConfigSpec *config) {
    for (size_t i = 0; i < std::size(kBuiltins); i++) {
        const Builtin &b = kBuiltins[i];
        if (indexOfId(config->integrations, b.id) >= 0) continue;
        IntegrationSpec spec;
        spec.id = b.id;
        spec.type = b.type;
        spec.name = b.name;
        spec.deviceName = b.deviceName;
        spec.enabled = b.enabled;
        config->integrations.insert(config->integrations.begin() + static_cast<std::ptrdiff_t>(i), spec);
    }
}

}  // namespace hubbo
