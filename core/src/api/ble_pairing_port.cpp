#include "hubbo/api/ble_pairing_port.h"

namespace hubbo::api {

const char *blePairStateName(BlePairState state) {
    switch (state) {
        case BlePairState::Pairing: return "pairing";
        case BlePairState::Paired: return "paired";
        case BlePairState::Failed: return "failed";
        case BlePairState::NotReady:
        case BlePairState::Idle: break;
    }
    return "idle";
}

BlePairingStatus SimulatedBlePairing::status() const {
    BlePairingStatus s = status_;
    if (!ready) s.state = BlePairState::NotReady;
    return s;
}

BlePairingResult SimulatedBlePairing::startPairing(uint32_t windowMs) {
    if (!ready) return BlePairingResult::NotReady;
    if (status_.state == BlePairState::Pairing) return BlePairingResult::Busy;
    status_ = BlePairingStatus{BlePairState::Pairing, windowMs, ""};  // the old bond goes first
    return BlePairingResult::Accepted;
}

BlePairingResult SimulatedBlePairing::forget() {
    if (!ready) return BlePairingResult::NotReady;
    status_ = BlePairingStatus{BlePairState::Idle, 0, ""};
    return BlePairingResult::Accepted;
}

void SimulatedBlePairing::advance(uint32_t elapsedMs) {
    if (status_.state != BlePairState::Pairing) return;
    if (elapsedMs < status_.windowRemainingMs) {
        status_.windowRemainingMs -= elapsedMs;
        return;
    }
    status_ = BlePairingStatus{BlePairState::Failed, 0, ""};
}

void SimulatedBlePairing::bonded(const std::string &peerName) {
    status_ = BlePairingStatus{BlePairState::Paired, 0, peerName.substr(0, 48)};
}

}  // namespace hubbo::api
