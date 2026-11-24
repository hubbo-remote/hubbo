// Ported from Harmonizer src/smart_hub.cpp and src/config/smart_hub.cpp
// (soaresbrun0/Harmonizer, AGPL-3.0).
#include "remote_radio.h"

#include <Arduino.h>
#include <Preferences.h>
#include <RF24.h>
#include <SPI.h>

#include "board.h"
#include "hubbo/api/remote_port.h"
#include "console.h"

namespace hubbo::fw {

namespace {

// Harmony Hub pairing handshake, unchanged from Harmonizer.
constexpr uint64_t kPairingAddress = 0xBB0ADCA575;
constexpr uint8_t kHubChannels[] = {5, 8, 14, 17, 32, 35, 41, 44, 62, 65, 71, 74};
constexpr size_t kHubChannelCount = sizeof(kHubChannels) / sizeof(kHubChannels[0]);
const uint8_t kPairMessage[22] = {242, 95, 1, 225, 154, 157, 218, 83, 40, 64, 30,
                                  4,   2,  7, 12,  0,   0,   0,   0,  0,  102, 100};
const uint8_t kPingMessage[5] = {242, 64, 1, 225, 236};
constexpr uint8_t kPingsPerPair = 10;
// Harmonizer waited 100 ms after each write before reading the ACK payload.
constexpr uint32_t kScanStepMs = 100;
constexpr uint8_t kHubReplyLength = 22;

constexpr const char *kPrefsNamespace = "hubbo-radio";
constexpr const char *kPrefsChannel = "channel";
constexpr const char *kPrefsAddress = "address";

RF24 radio(board::nrf24::kCe, board::nrf24::kCsn);

bool loadEndpoint(RadioEndpoint *out) {
    Preferences prefs;
    if (!prefs.begin(kPrefsNamespace, true)) {
        return false;  // namespace doesn't exist yet
    }
    RadioEndpoint endpoint;
    endpoint.channel = prefs.getUChar(kPrefsChannel, 0xFF);
    endpoint.address = prefs.getULong64(kPrefsAddress, 0);
    prefs.end();
    if (!endpoint.isValid()) {
        return false;
    }
    *out = endpoint;
    return true;
}

void saveEndpoint(const RadioEndpoint &endpoint) {
    Preferences prefs;
    prefs.begin(kPrefsNamespace, false);
    prefs.putUChar(kPrefsChannel, endpoint.channel);
    prefs.putULong64(kPrefsAddress, endpoint.address);
    prefs.end();
}

void eraseEndpoint() {
    Preferences prefs;
    prefs.begin(kPrefsNamespace, false);
    prefs.clear();
    prefs.end();
}

}  // namespace

bool RadioEndpoint::isValid() const {
    return hubbo::api::isValidRemoteEndpoint({channel, address});
}

void RemoteRadio::persistEndpoint(const RadioEndpoint &endpoint, bool erase) {
    if (erase) {
        eraseEndpoint();
    } else {
        saveEndpoint(endpoint);
    }
}

void RemoteRadio::persist(const RadioEndpoint &endpoint, bool erase) {
    if (persist_ != nullptr) {
        persist_(endpoint, erase);
    } else {
        persistEndpoint(endpoint, erase);
    }
}

const char *RemoteRadio::stateName(State state) {
    switch (state) {
        case State::NotReady: return "not_ready";
        case State::Idle: return "idle";
        case State::Scanning: return "scanning";
        case State::Listening: return "listening";
    }
    return "not_ready";
}

uint32_t RemoteRadio::scanRemainingMs(uint32_t nowMs) const {
    if (state_ != State::Scanning) {
        return 0;
    }
    const uint32_t elapsed = nowMs - scanStartedMs_;
    return elapsed >= scanTimeoutMs_ ? 0 : scanTimeoutMs_ - elapsed;
}

uint8_t RemoteRadio::scanChannel() const {
    return kHubChannels[channelIndex_];
}

bool RemoteRadio::begin(config::RadioPower power) {
    if (state_ != State::NotReady) {
        return true;
    }
    SPI.begin(board::nrf24::kSck, board::nrf24::kMiso, board::nrf24::kMosi, board::nrf24::kCsn);
    if (!radio.begin(&SPI)) {
        con.println("radio: nRF24L01+ not responding");
        return false;
    }
    radio.setDataRate(RF24_2MBPS);
    radio.enableDynamicPayloads();
    radio.enableAckPayload();
    radio.setCRCLength(RF24_CRC_16);
    applyPower(power);
    state_ = State::Idle;
    con.printf("radio: nRF24L01+ ready (%s, power %s)\n", radio.isPVariant() ? "+ variant" : "non-+",
               config::radioPowerName(power));

    RadioEndpoint saved;
    if (loadEndpoint(&saved)) {
        listen(saved);
    } else {
        con.println("radio: no Hub endpoint saved; send `scan` and press the Hub's pair button");
    }
    return true;
}

void RemoteRadio::loop(uint32_t nowMs) {
    switch (state_) {
        case State::NotReady:
        case State::Idle:
            break;
        case State::Scanning:
            scanStep(nowMs);
            break;
        case State::Listening:
            drain();
            break;
    }
}

void RemoteRadio::applyPower(config::RadioPower power) {
    static constexpr uint8_t kLevels[] = {RF24_PA_MIN, RF24_PA_LOW, RF24_PA_HIGH, RF24_PA_MAX};
    radio.setPALevel(kLevels[static_cast<uint8_t>(power)], true);
    power_ = power;
}

void RemoteRadio::setPower(config::RadioPower power) {
    if (state_ == State::NotReady || power == power_) {
        return;
    }
    // W_REGISTER is for standby only (datasheet): drop out of RX around it.
    const bool listening = state_ == State::Listening;
    if (listening) {
        radio.ce(LOW);
    }
    applyPower(power);
    if (listening) {
        radio.ce(HIGH);
    }
    con.printf("radio: power %s\n", config::radioPowerName(power));
}

// --- Listening -----------------------------------------------------------

void RemoteRadio::listen(const RadioEndpoint &endpoint) {
    // Data rate, dynamic payloads and CRC are set once in begin().
    radio.setChannel(endpoint.channel);
    radio.openReadingPipe(1, endpoint.address & 0xFFFFFFFF00ULL);
    radio.openReadingPipe(2, endpoint.address & 0xFFFFFFFFFFULL);
    radio.startListening();
    endpoint_ = endpoint;
    state_ = State::Listening;
    con.printf("radio: listening on channel %u, address 0x%010llX\n",
                  static_cast<unsigned>(endpoint.channel),
                  static_cast<unsigned long long>(endpoint.address));
}

void RemoteRadio::stopListening() {
    radio.stopListening();
    radio.flush_rx();
    state_ = State::Idle;
    sink_.onListeningStopped();
}

void RemoteRadio::drain() {
    uint8_t pipe = 0;
    while (radio.available(&pipe)) {
        uint8_t bytes[32];
        const uint8_t length = radio.getDynamicPayloadSize();
        if (length == 0 || length > sizeof(bytes)) {
            // RF24 already flushed a corrupt payload.
            oversizeCount_++;
            continue;
        }
        radio.read(bytes, length);
        const uint32_t readUs = micros();
        payloadCount_++;
        lastPayloadMs_ = millis();
        sink_.onRadioPayload(bytes, length, pipe, readUs);
    }
}

void RemoteRadio::forgetEndpoint() {
    if (state_ == State::Scanning) {
        stopScan();
    }
    if (state_ == State::Listening) {
        stopListening();
    }
    endpoint_ = {};
    if (state_ != State::NotReady) {
        persist(endpoint_, true);
    }
    con.println("radio: Hub endpoint forgotten");
}

bool RemoteRadio::setEndpoint(const RadioEndpoint &endpoint) {
    if (state_ == State::NotReady || !endpoint.isValid()) {
        return false;
    }
    if (state_ == State::Scanning) {
        stopScan();
    }
    if (state_ == State::Listening) {
        stopListening();  // releases held buttons (PayloadSink::onListeningStopped)
    }
    persist(endpoint, false);
    listen(endpoint);
    return true;
}

// --- Scanning --------------------------------------------------------------

bool RemoteRadio::startScan(uint32_t nowMs, uint32_t timeoutMs) {
    if (state_ == State::NotReady || state_ == State::Scanning) {
        return false;
    }
    if (state_ == State::Listening) {
        stopListening();  // leaves the radio in TX mode, as begin() does
    }
    radio.flush_tx();
    radio.openWritingPipe(kPairingAddress);
    channelIndex_ = 0;
    pingsRemaining_ = 0;
    scanStepPending_ = false;
    scanStartedMs_ = nowMs;
    scanTimeoutMs_ = timeoutMs;
    lastScanStepMs_ = nowMs - kScanStepMs;
    state_ = State::Scanning;
    con.println("radio: scanning for a Harmony Hub; press its pair button");
    return true;
}

void RemoteRadio::stopScan() {
    if (state_ == State::Scanning) {
        finishScan(ScanResult::Stopped);
    }
}

void RemoteRadio::finishScan(ScanResult result) {
    lastScanResult_ = result;
    state_ = State::Idle;
    switch (result) {
        case ScanResult::TimedOut: con.println("radio: scan timed out, no Hub found"); break;
        case ScanResult::Stopped: con.println("radio: scan stopped"); break;
        default: break;
    }
    if (endpoint_.isValid()) {
        listen(endpoint_);
    }
}

// One step of Harmonizer's scan loop per kScanStepMs: read the ACK payload
// answering the previous write, then write the next pair/ping message.
// radio.write() blocks for its auto-retries: up to ~25 ms when nothing
// acknowledges (RF24 default 15 retries x 1.5 ms). Scanning is a one-off setup step.
void RemoteRadio::scanStep(uint32_t nowMs) {
    if (nowMs - lastScanStepMs_ < kScanStepMs) {
        return;
    }
    lastScanStepMs_ = nowMs;

    if (scanStepPending_ && radio.isAckPayloadAvailable()) {
        uint8_t reply[32];
        const uint8_t length = radio.getDynamicPayloadSize();
        if (length > 0 && length <= sizeof(reply)) {
            radio.read(reply, length);
        }
        if (length == kHubReplyLength) {
            RadioEndpoint found;
            found.channel = kHubChannels[channelIndex_];
            found.address = static_cast<uint64_t>(reply[3]) << 32 |
                            static_cast<uint64_t>(reply[4]) << 24 |
                            static_cast<uint64_t>(reply[5]) << 16 |
                            static_cast<uint64_t>(reply[6]) << 8 |
                            static_cast<uint64_t>(static_cast<uint8_t>(reply[7] - 1));
            if (found.isValid()) {
                con.println("radio: found Harmony Hub");
                endpoint_ = found;
                persist(found, false);
                finishScan(ScanResult::Found);
                return;
            }
            con.println("radio: Hub reply had an invalid address, still scanning");
        }
    }

    if (nowMs - scanStartedMs_ >= scanTimeoutMs_) {
        finishScan(ScanResult::TimedOut);
        return;
    }

    if (pingsRemaining_ == 0) {
        radio.setChannel(kHubChannels[channelIndex_]);
        if (radio.write(kPairMessage, sizeof(kPairMessage))) {
            pingsRemaining_ = kPingsPerPair;
        } else {
            channelIndex_ = static_cast<uint8_t>((channelIndex_ + 1) % kHubChannelCount);
        }
    } else {
        radio.write(kPingMessage, sizeof(kPingMessage));
        pingsRemaining_--;
    }
    scanStepPending_ = true;
}

}  // namespace hubbo::fw
