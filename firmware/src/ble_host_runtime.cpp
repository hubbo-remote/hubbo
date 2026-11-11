#include "ble_host_runtime.h"

namespace hubbo::fw {

using hubbo::integrations::BleHostResult;
using hubbo::integrations::BleDeviceIds;
using hubbo::integrations::BleHostStatus;

// The same rules BleHid applies when the loop runs the requests: one
// window at a time, closed by a cancel or forget of its device, or by a
// pending set that leaves its device out (applied after the queue).
BleHostRuntime::Window BleHostRuntime::windowLocked() const {
    Window window;
    for (size_t i = 0; i < snapshot_.count; i++) {
        if (snapshot_.entries[i].status.pairing) {
            window.device = snapshot_.entries[i].deviceId;
            window.remainingMs = snapshot_.entries[i].status.windowRemainingMs;
        }
    }
    for (size_t i = 0; i < requestCount_; i++) {
        const Request &request = requests_[i];
        if (request.kind == Kind::Start) {
            window.device = request.deviceId;
            window.remainingMs = request.windowMs;
        } else if (window.device == request.deviceId.view()) {
            window = Window{};  // its cancel or forget
        }
    }
    if (retainPending_ && !window.device.empty() && !retain_.contains(window.device.view())) {
        window = Window{};
    }
    return window;
}

BleHostStatus BleHostRuntime::statusLocked(std::string_view deviceId) const {
    BleHostStatus status;
    for (size_t i = 0; i < snapshot_.count; i++) {
        if (snapshot_.entries[i].deviceId == deviceId) {
            status = snapshot_.entries[i].status;
            break;
        }
    }
    for (size_t i = 0; i < requestCount_; i++) {
        if (requests_[i].kind == Kind::Forget && requests_[i].deviceId == deviceId) {
            status = BleHostStatus{};
        }
    }
    if (retainPending_ && !retain_.contains(deviceId)) {
        status = BleHostStatus{};
    }
    const Window window = windowLocked();
    status.pairing = !deviceId.empty() && window.device == deviceId;
    status.windowRemainingMs = status.pairing ? window.remainingMs : 0;
    return status;
}

bool BleHostRuntime::radioReady() const {
    portENTER_CRITICAL(&lock_);
    const bool ready = snapshot_.radioReady;
    portEXIT_CRITICAL(&lock_);
    return ready;
}

BleHostStatus BleHostRuntime::status(std::string_view deviceId) const {
    portENTER_CRITICAL(&lock_);
    const BleHostStatus status = statusLocked(deviceId);
    portEXIT_CRITICAL(&lock_);
    return status;
}

BleHostResult BleHostRuntime::enqueue(Kind kind, std::string_view deviceId, uint32_t windowMs) {
    Request request;
    request.kind = kind;
    request.windowMs = windowMs;
    if (deviceId.empty() || !request.deviceId.assign(deviceId)) {
        return BleHostResult::Accepted;  // no such device: nothing to do
    }
    BleHostResult result = BleHostResult::Accepted;
    bool queue = true;
    portENTER_CRITICAL(&lock_);
    if (!snapshot_.radioReady) {
        result = BleHostResult::NotReady;
    } else if (kind == Kind::Start && statusLocked(deviceId).pairing) {
        queue = false;  // its window is already open: no change
    } else if (kind == Kind::Start && !windowLocked().device.empty()) {
        result = BleHostResult::Busy;
    } else if (requestCount_ == kMaxRequests) {
        result = BleHostResult::Full;
    }
    if (result == BleHostResult::Accepted && queue) {
        requests_[requestCount_++] = request;
    }
    portEXIT_CRITICAL(&lock_);
    return result;
}

BleHostResult BleHostRuntime::startPairing(std::string_view deviceId, uint32_t windowMs) {
    return enqueue(Kind::Start, deviceId, windowMs);
}

BleHostResult BleHostRuntime::cancelPairing(std::string_view deviceId) {
    return enqueue(Kind::Cancel, deviceId, 0);
}

BleHostResult BleHostRuntime::forget(std::string_view deviceId) {
    return enqueue(Kind::Forget, deviceId, 0);
}

void BleHostRuntime::retainOnly(const hubbo::integrations::BleDeviceIds &devices) {
    portENTER_CRITICAL(&lock_);
    retain_ = devices;
    retainPending_ = true;
    retainSeq_++;
    portEXIT_CRITICAL(&lock_);
}

void BleHostRuntime::poll(uint32_t nowMs) {
    Request requests[kMaxRequests];
    BleDeviceIds retain;
    uint32_t retainSeq = 0;
    portENTER_CRITICAL(&lock_);
    const size_t count = requestCount_;
    for (size_t i = 0; i < count; i++) {
        requests[i] = requests_[i];
    }
    // Only once the radio is up; until then it stays pending (and shown).
    const bool retaining = retainPending_ && ble_.started();
    if (retaining) {
        retain = retain_;
        retainSeq = retainSeq_;
    }
    portEXIT_CRITICAL(&lock_);
    if (count == 0 && !retaining) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        const Request &request = requests[i];
        const std::string_view deviceId = request.deviceId.view();
        switch (request.kind) {
            case Kind::Start: ble_.startPairing(deviceId, nowMs, request.windowMs); break;
            case Kind::Cancel: ble_.cancelPairing(deviceId); break;
            case Kind::Forget: ble_.forget(deviceId); break;
        }
    }
    if (retaining) {
        ble_.retainOnly(retain);
    }
    store(nowMs, count, retaining, retainSeq);
}

void BleHostRuntime::publish(uint32_t nowMs) {
    store(nowMs, 0, false, 0);
}

// The new state and the dropped requests (and the applied set, unless a
// newer one came in) land together, so status() never shows a state without
// both.
void BleHostRuntime::store(uint32_t nowMs, size_t ran, bool retained, uint32_t retainSeq) {
    BleHid::Snapshot fresh;
    ble_.snapshot(fresh, nowMs);
    portENTER_CRITICAL(&lock_);
    snapshot_ = fresh;
    // Requests queued while these ran stay, in order.
    for (size_t i = ran; i < requestCount_; i++) {
        requests_[i - ran] = requests_[i];
    }
    requestCount_ -= ran;
    if (retained && retainSeq == retainSeq_) {
        retainPending_ = false;
    }
    portEXIT_CRITICAL(&lock_);
}

}  // namespace hubbo::fw
