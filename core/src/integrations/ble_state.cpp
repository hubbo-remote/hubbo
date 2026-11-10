#include "hubbo/integrations/ble_state.h"

namespace hubbo::integrations {

using api::BlePairState;

const char *bleIntegrationState(const IntegrationSpec &integration, BlePairState pairing, bool hostConnected) {
    if (!integration.enabled) return "disabled";
    switch (pairing) {
        case BlePairState::NotReady: return "error";
        case BlePairState::Pairing: return "connecting";
        case BlePairState::Paired: return hostConnected ? "connected" : "disconnected";
        case BlePairState::Idle:
        case BlePairState::Failed: break;
    }
    return "not_configured";
}

std::string bleIntegrationDetail(BlePairState pairing, bool hostConnected, std::string_view peerName) {
    switch (pairing) {
        case BlePairState::NotReady: return "Bluetooth isn't working";
        case BlePairState::Pairing: return "Ready to pair";
        case BlePairState::Paired: {
            std::string detail = hostConnected ? "Connected to " : "Waiting for ";
            detail.append(peerName.empty() ? std::string_view("your device") : peerName);
            return detail;
        }
        case BlePairState::Idle:
        case BlePairState::Failed: break;
    }
    return "Not paired";
}

const char *bleDeviceAvailability(BlePairState pairing, bool hostConnected) {
    return pairing == BlePairState::Paired && hostConnected ? "available" : "unavailable";
}

}  // namespace hubbo::integrations
