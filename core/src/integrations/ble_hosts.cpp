#include "hubbo/integrations/ble_hosts.h"

namespace hubbo::integrations {

bool BleDeviceIds::contains(std::string_view deviceId) const {
    for (size_t i = 0; i < count; i++) {
        if (ids[i] == deviceId) return true;
    }
    return false;
}

bool bleDeviceIds(const ConfigSpec &config, BleDeviceIds *out) {
    *out = {};
    for (const DeviceSpec &device : config.devices) {
        if (device.provider.type != kBleHidType) continue;
        if (out->count == kMaxBleDevices || !out->ids[out->count].assign(device.id)) return false;
        out->count++;
    }
    return true;
}

void BleHostReconciler::configSaved(const ConfigSpec &config) {
    BleDeviceIds devices;
    if (bleDeviceIds(config, &devices)) port_.retainOnly(devices);
}

BleDeviceState bleDeviceState(const BleHostStatus &status) {
    if (status.pairing) return {"pairing", (status.windowRemainingMs + 999) / 1000};
    if (!status.bonded) return {"not_paired", 0};
    return {status.connected ? "connected" : "not_connected", 0};
}

const char *bleDeviceAvailability(const BleHostStatus &status) {
    return status.bonded && status.connected ? "available" : "unavailable";
}

const char *bleRadioState(const IntegrationSpec &integration, bool radioReady) {
    if (!integration.enabled) return "disabled";
    return radioReady ? "connected" : "error";
}

std::string bleRadioDetail(bool radioReady) { return radioReady ? std::string() : "Bluetooth isn't working"; }

}  // namespace hubbo::integrations
