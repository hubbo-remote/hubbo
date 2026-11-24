#include "remote_runtime.h"

namespace hubbo::fw {

using hubbo::api::RadioState;
using hubbo::api::RemoteResult;
using hubbo::api::ScanState;

hubbo::api::RemoteRadioStatus RemoteRuntime::status() const {
    portENTER_CRITICAL(&lock_);
    hubbo::api::RemoteRadioStatus copy = status_;
    if (request_ == Request::StartScan) {  // not taken by the loop yet: show it as started
        copy.radio = RadioState::Scanning;
        copy.scan = ScanState::Scanning;
        copy.scanRemainingMs = requestTimeoutMs_;
    }
    portEXIT_CRITICAL(&lock_);
    return copy;
}

RemoteResult RemoteRuntime::post(Request request, const RadioEndpoint &endpoint, uint32_t timeoutMs) {
    RemoteResult result = RemoteResult::Accepted;
    portENTER_CRITICAL(&lock_);
    if (status_.radio == RadioState::NotReady) {
        result = RemoteResult::NotReady;
    } else if (status_.radio == RadioState::Scanning || request_ != Request::None) {
        result = RemoteResult::Busy;  // scanning, or a change not yet taken by the loop
    } else {
        request_ = request;
        requestEndpoint_ = endpoint;
        requestTimeoutMs_ = timeoutMs;
    }
    portEXIT_CRITICAL(&lock_);
    return result;
}

RemoteResult RemoteRuntime::setEndpoint(const hubbo::api::RemoteEndpoint &endpoint) {
    if (!hubbo::api::isValidRemoteEndpoint(endpoint)) {
        return RemoteResult::Invalid;
    }
    RadioEndpoint radioEndpoint;
    radioEndpoint.channel = endpoint.channel;
    radioEndpoint.address = endpoint.address;
    return post(Request::SetEndpoint, radioEndpoint, 0);
}

RemoteResult RemoteRuntime::startScan(uint32_t timeoutMs) {
    return post(Request::StartScan, RadioEndpoint{}, timeoutMs);
}

void RemoteRuntime::stopScan() {
    portENTER_CRITICAL(&lock_);
    if (status_.radio == RadioState::Scanning || request_ == Request::StartScan) {
        request_ = Request::StopScan;  // replaces a start not yet taken
    }
    portEXIT_CRITICAL(&lock_);
}

void RemoteRuntime::poll(uint32_t nowMs) {
    portENTER_CRITICAL(&lock_);
    const Request request = request_;
    const RadioEndpoint endpoint = requestEndpoint_;
    const uint32_t timeoutMs = requestTimeoutMs_;
    request_ = Request::None;
    portEXIT_CRITICAL(&lock_);
    switch (request) {
        case Request::None: return;
        case Request::SetEndpoint: radio_.setEndpoint(endpoint); break;
        case Request::StartScan: radio_.startScan(nowMs, timeoutMs); break;
        case Request::StopScan: radio_.stopScan(); break;
    }
    publish(nowMs);  // status() shows the effect at once
}

void RemoteRuntime::publish(uint32_t nowMs) {
    hubbo::api::RemoteRadioStatus s;
    s.radio = radio_.state();
    const RadioEndpoint endpoint = radio_.endpoint();
    s.hasEndpoint = endpoint.isValid();
    if (s.hasEndpoint) {
        s.endpoint.channel = endpoint.channel;
        s.endpoint.address = endpoint.address;
    }
    if (s.radio == RadioState::Scanning) {
        s.scan = ScanState::Scanning;
        s.scanRemainingMs = radio_.scanRemainingMs(nowMs);
        s.scanChannel = radio_.scanChannel();
    } else {
        s.scan = radio_.lastScanResult();
    }
    portENTER_CRITICAL(&lock_);
    status_ = s;
    portEXIT_CRITICAL(&lock_);
}

}  // namespace hubbo::fw
