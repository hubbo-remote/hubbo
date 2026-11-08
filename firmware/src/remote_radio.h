#pragma once

// nRF24 link to a Harmony Companion remote.
//
// Hubbo listens on the radio address of an existing Harmony Hub. The address
// is learned once by scanning while the Hub's pair button is pressed, then
// kept in NVS. Every received payload goes to a PayloadSink untouched; the
// portable core (hubbo::remote::RemoteInput) does all decoding.
//
// Ported from Harmonizer's src/smart_hub.cpp (soaresbrun0/Harmonizer,
// AGPL-3.0). The scan is the same handshake, made non-blocking.
//
// All methods are for the main loop only.

#include <cstddef>
#include <cstdint>

namespace hubbo::fw {

struct RadioEndpoint {
    uint8_t channel = 0;
    uint64_t address = 0;  // 40-bit nRF24 address

    bool isValid() const;
};

class PayloadSink {
public:
    virtual ~PayloadSink() = default;
    // `readUs` is micros() right after the payload left the radio's FIFO.
    virtual void onRadioPayload(const uint8_t *bytes, uint8_t length, uint8_t pipe,
                                uint32_t readUs) = 0;
    // Listening stopped (scan, forget): no release packet will arrive for
    // anything held, so release it now.
    virtual void onListeningStopped() = 0;
};

class RemoteRadio {
public:
    enum class State : uint8_t {
        NotReady,   // radio not started or not responding
        Idle,       // no Hub endpoint known
        Scanning,   // looking for a Hub in pairing mode
        Listening,  // receiving the remote
    };

    enum class ScanResult : uint8_t { None, Found, TimedOut, Stopped };

    explicit RemoteRadio(PayloadSink &sink) : sink_(sink) {}

    // Starts SPI and the radio, loads the saved endpoint and starts
    // listening if there is one. False if the radio does not respond.
    bool begin();

    // Call every loop iteration: drains the RX FIFO, or runs one scan step.
    void loop(uint32_t nowMs);

    // Scan for a Hub (press its pair button). Interrupts listening; on
    // success the endpoint is saved and listening resumes on it, otherwise
    // listening resumes on the previous endpoint, if any.
    bool startScan(uint32_t nowMs, uint32_t timeoutMs = kDefaultScanTimeoutMs);
    void stopScan();

    // Stops listening and erases the saved endpoint.
    void forgetEndpoint();

    State state() const { return state_; }
    static const char *stateName(State state);
    RadioEndpoint endpoint() const { return endpoint_; }
    ScanResult lastScanResult() const { return lastScanResult_; }
    uint8_t scanChannel() const;

    uint32_t payloadCount() const { return payloadCount_; }
    uint32_t lastPayloadMs() const { return lastPayloadMs_; }
    uint32_t oversizeCount() const { return oversizeCount_; }

    static constexpr uint32_t kDefaultScanTimeoutMs = 120000;

private:
    void listen(const RadioEndpoint &endpoint);
    void stopListening();
    void scanStep(uint32_t nowMs);
    void finishScan(ScanResult result);
    void drain();

    PayloadSink &sink_;
    State state_ = State::NotReady;
    RadioEndpoint endpoint_;
    ScanResult lastScanResult_ = ScanResult::None;

    uint32_t scanStartedMs_ = 0;
    uint32_t scanTimeoutMs_ = 0;
    uint32_t lastScanStepMs_ = 0;
    bool scanStepPending_ = false;
    uint8_t channelIndex_ = 0;
    uint8_t pingsRemaining_ = 0;

    uint32_t payloadCount_ = 0;
    uint32_t lastPayloadMs_ = 0;
    uint32_t oversizeCount_ = 0;
};

}  // namespace hubbo::fw
