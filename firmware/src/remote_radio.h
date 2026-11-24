#pragma once

// nRF24 link to a Harmony Companion remote.
//
// Hubbo listens on the radio address of an existing Harmony Hub. The address
// is learned once by scanning while the Hub's pair button is pressed, then
// kept in NVS (read at boot; written off the RF task, see setPersistRunner). Every received payload goes to a PayloadSink untouched; the
// portable core (hubbo::remote::RemoteInput) does all decoding.
//
// Ported from Harmonizer's src/smart_hub.cpp (soaresbrun0/Harmonizer,
// AGPL-3.0). The scan is the same handshake, made non-blocking.
//
// All methods are for the main loop only.

#include <cstddef>
#include <cstdint>

#include "hubbo/api/remote_port.h"
#include "hubbo/config/hub.h"
#include "hubbo/remote/packet.h"

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
    // `signal`: Strong/Weak from the RPD (-64 dBm), Unknown without one (a
    // non-+ radio, a release build). It latches on the latest packet, so
    // payloads queued in the FIFO together share it.
    virtual void onRadioPayload(const uint8_t *bytes, uint8_t length, uint8_t pipe,
                                uint32_t readUs, remote::Signal signal) = 0;
    // Listening stopped (scan, forget): no release packet will arrive for
    // anything held, so release it now.
    virtual void onListeningStopped() = 0;
};

class RemoteRadio {
public:
    explicit RemoteRadio(PayloadSink &sink) : sink_(sink) {}

    // Where saving/erasing the endpoint in NVS runs. NVS can block on its
    // lock, so the RF task must never call it: main hands it to httpd. The
    // job owns its copy of the endpoint (`erase` true: forget it). Unset:
    // done inline (boot, before other tasks run).
    using PersistRunner = void (*)(const RadioEndpoint &endpoint, bool erase);
    void setPersistRunner(PersistRunner runner) { persist_ = runner; }

    // Does the NVS write itself; the runner's job calls this.
    static void persistEndpoint(const RadioEndpoint &endpoint, bool erase);

    // Starts SPI and the radio at `power`, loads the saved endpoint and
    // starts listening if there is one. False if the radio does not respond.
    bool begin(config::RadioPower power);

    // The transmit power (auto-acks and the scan's pairing writes); the LNA
    // stays on. Call every pass with the saved setting: unchanged is a no-op.
    void setPower(config::RadioPower power);
    config::RadioPower power() const { return power_; }

    // Call every loop iteration: drains the RX FIFO, or runs one scan step.
    void loop(uint32_t nowMs);

    // Scan for a Hub (press its pair button). Interrupts listening; on
    // success the endpoint is saved and listening resumes on it, otherwise
    // listening resumes on the previous endpoint, if any.
    bool startScan(uint32_t nowMs, uint32_t timeoutMs);
    void stopScan();

    // Stops listening and erases the saved endpoint.
    void forgetEndpoint();

    // Uses a known endpoint instead of scanning (e.g. a Hub that's gone, its
    // channel and address noted earlier). Saved like a scanned one and
    // listened on at once; held buttons are released first. False if the
    // radio isn't ready or the endpoint is invalid.
    bool setEndpoint(const RadioEndpoint &endpoint);

    api::RadioState state() const { return state_; }
    RadioEndpoint endpoint() const { return endpoint_; }
    api::ScanState lastScanResult() const { return lastScanResult_; }
    uint8_t scanChannel() const;
    uint32_t scanRemainingMs(uint32_t nowMs) const;  // 0 unless scanning

    uint32_t payloadCount() const { return payloadCount_; }
    uint32_t lastPayloadMs() const { return lastPayloadMs_; }


private:
    void listen(const RadioEndpoint &endpoint);
    void stopListening();
    void scanStep(uint32_t nowMs);
    void finishScan(api::ScanState result);
    void drain();
    remote::Signal readSignal() const;

    void persist(const RadioEndpoint &endpoint, bool erase);
    void applyPower(config::RadioPower power);

    PayloadSink &sink_;
    PersistRunner persist_ = nullptr;
    api::RadioState state_ = api::RadioState::NotReady;
    RadioEndpoint endpoint_;
    config::RadioPower power_ = config::RadioPower::Max;  // RF24's reset default
    bool hasRpd_ = false;  // an nRF24L01+ (RPD needs the + variant)
    api::ScanState lastScanResult_ = api::ScanState::None;  // never Scanning

    uint32_t scanStartedMs_ = 0;
    uint32_t scanTimeoutMs_ = 0;
    uint32_t lastScanStepMs_ = 0;
    bool scanStepPending_ = false;
    uint8_t channelIndex_ = 0;
    uint8_t pingsRemaining_ = 0;

    uint32_t payloadCount_ = 0;
    uint32_t lastPayloadMs_ = 0;
};

}  // namespace hubbo::fw
