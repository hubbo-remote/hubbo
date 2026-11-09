#include "hubbo/api/remote_port.h"

#include <cstdio>

namespace hubbo::api {

bool parseRemoteAddress(std::string_view text, uint64_t *address) {
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.remove_prefix(2);
    if (text.empty() || text.size() > 10) return false;
    uint64_t value = 0;
    for (char c : text) {
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        if (digit < 0) return false;
        value = value * 16 + static_cast<uint64_t>(digit);
    }
    *address = value;
    return true;
}

std::string formatRemoteAddress(uint64_t address) {
    char text[11];
    std::snprintf(text, sizeof(text), "%010llX", static_cast<unsigned long long>(address & 0xFFFFFFFFFFULL));
    return text;
}

bool isValidRemoteEndpoint(const RemoteEndpoint &endpoint) {
    if (endpoint.channel > kMaxRemoteChannel) return false;
    if (endpoint.address > 0xFFFFFFFFFFULL) return false;
    if (endpoint.address == 0 || endpoint.address == 0xFFFFFFFFFFULL) return false;
    const auto firstByte = static_cast<uint8_t>(endpoint.address & 0xFF);
    return firstByte != 0x55 && firstByte != 0xAA;
}

const char *radioStateName(RadioState state) {
    switch (state) {
        case RadioState::NotReady: return "not_ready";
        case RadioState::Idle: return "idle";
        case RadioState::Scanning: return "scanning";
        case RadioState::Listening: return "listening";
    }
    return "not_ready";
}

const char *scanStateName(ScanState state) {
    switch (state) {
        case ScanState::None: return "idle";
        case ScanState::Scanning: return "scanning";
        case ScanState::Found: return "found";
        case ScanState::TimedOut: return "timeout";
        case ScanState::Stopped: return "stopped";
    }
    return "idle";
}

RemoteRadioStatus SimulatedRemote::status() const {
    RemoteRadioStatus s = status_;
    if (!ready) s.radio = RadioState::NotReady;
    return s;
}

RemoteResult SimulatedRemote::setEndpoint(const RemoteEndpoint &endpoint) {
    if (!isValidRemoteEndpoint(endpoint)) return RemoteResult::Invalid;
    if (!ready) return RemoteResult::NotReady;
    if (status_.radio == RadioState::Scanning) return RemoteResult::Busy;
    status_.hasEndpoint = true;
    status_.endpoint = endpoint;
    status_.radio = RadioState::Listening;
    return RemoteResult::Accepted;
}

RemoteResult SimulatedRemote::startScan(uint32_t timeoutMs) {
    if (!ready) return RemoteResult::NotReady;
    if (status_.radio == RadioState::Scanning) return RemoteResult::Busy;
    status_.radio = RadioState::Scanning;
    status_.scan = ScanState::Scanning;
    status_.scanRemainingMs = timeoutMs;
    status_.scanChannel = 0;
    return RemoteResult::Accepted;
}

void SimulatedRemote::stopScan() {
    if (status_.radio != RadioState::Scanning) return;
    status_.scan = ScanState::Stopped;
    status_.scanRemainingMs = 0;
    status_.radio = status_.hasEndpoint ? RadioState::Listening : RadioState::Idle;
}

void SimulatedRemote::advance(uint32_t elapsedMs) {
    if (status_.radio != RadioState::Scanning) return;
    status_.scanChannel = static_cast<uint8_t>((status_.scanChannel + elapsedMs / 100) % (kMaxRemoteChannel + 1));
    if (elapsedMs < status_.scanRemainingMs) {
        status_.scanRemainingMs -= elapsedMs;
        return;
    }
    status_.scanRemainingMs = 0;
    status_.scan = ScanState::TimedOut;
    status_.radio = status_.hasEndpoint ? RadioState::Listening : RadioState::Idle;
}

void SimulatedRemote::found(const RemoteEndpoint &endpoint) {
    if (status_.radio != RadioState::Scanning || !isValidRemoteEndpoint(endpoint)) return;
    status_.scan = ScanState::Found;
    status_.scanRemainingMs = 0;
    status_.hasEndpoint = true;
    status_.endpoint = endpoint;
    status_.radio = RadioState::Listening;
}

}  // namespace hubbo::api
