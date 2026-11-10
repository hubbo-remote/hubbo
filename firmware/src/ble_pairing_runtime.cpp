#include "ble_pairing_runtime.h"

#include <cstdio>

namespace hubbo::fw {

using hubbo::api::BlePairingResult;
using hubbo::api::BlePairState;

hubbo::api::BlePairingStatus BlePairingRuntime::status() const {
    portENTER_CRITICAL(&lock_);
    Snapshot copy = snapshot_;
    const Request request = request_;
    const uint32_t windowMs = requestWindowMs_;
    portEXIT_CRITICAL(&lock_);

    // A request the loop hasn't taken yet: show its effect already.
    if (copy.state != BlePairState::NotReady) {
        if (request == Request::Start) {
            copy.state = BlePairState::Pairing;
            copy.windowRemainingMs = windowMs;
        } else if (request == Request::Forget) {
            copy.state = BlePairState::Idle;
        }
    }
    hubbo::api::BlePairingStatus status;
    status.state = copy.state;
    if (status.state == BlePairState::Pairing) {
        status.windowRemainingMs = copy.windowRemainingMs;
    } else if (status.state == BlePairState::Paired) {
        status.peerName = copy.peer;
    }
    return status;
}

BlePairingResult BlePairingRuntime::startPairing(uint32_t windowMs) {
    BlePairingResult result = BlePairingResult::Accepted;
    portENTER_CRITICAL(&lock_);
    if (snapshot_.state == BlePairState::NotReady) {
        result = BlePairingResult::NotReady;
    } else if (request_ == Request::Start ||
               (snapshot_.state == BlePairState::Pairing && request_ != Request::Forget)) {
        result = BlePairingResult::Busy;  // a window is open, or about to be
    } else {
        request_ = Request::Start;  // replaces a pending forget: a start forgets first
        requestWindowMs_ = windowMs;
    }
    portEXIT_CRITICAL(&lock_);
    return result;
}

BlePairingResult BlePairingRuntime::forget() {
    BlePairingResult result = BlePairingResult::Accepted;
    portENTER_CRITICAL(&lock_);
    if (snapshot_.state == BlePairState::NotReady) {
        result = BlePairingResult::NotReady;
    } else {
        request_ = Request::Forget;  // replaces a pending start
    }
    portEXIT_CRITICAL(&lock_);
    return result;
}

bool BlePairingRuntime::poll(uint32_t nowMs) {
    portENTER_CRITICAL(&lock_);
    const Request request = request_;
    const uint32_t windowMs = requestWindowMs_;
    request_ = Request::None;
    portEXIT_CRITICAL(&lock_);
    switch (request) {
        case Request::None: return false;
        case Request::Start: ble_.startPairing(nowMs, windowMs); break;  // forgets the old bond first
        case Request::Forget: ble_.forget(); break;
    }
    return true;
}

void BlePairingRuntime::publish(uint32_t nowMs, const char *peer) {
    Snapshot s;
    switch (ble_.state()) {
        case BleHid::State::Off:
            s.state = BlePairState::NotReady;
            break;
        case BleHid::State::Pairing:
            s.state = BlePairState::Pairing;
            s.windowRemainingMs = ble_.pairingSecondsLeft(nowMs) * 1000;
            break;
        case BleHid::State::Waiting:
        case BleHid::State::Connected:
            s.state = BlePairState::Paired;
            snprintf(s.peer, sizeof(s.peer), "%s", peer);
            break;
        case BleHid::State::NotPaired:
            s.state = ble_.pairingFailed() ? BlePairState::Failed : BlePairState::Idle;
            break;
    }
    portENTER_CRITICAL(&lock_);
    snapshot_ = s;
    portEXIT_CRITICAL(&lock_);
}

}  // namespace hubbo::fw
