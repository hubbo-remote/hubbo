#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The remote radio as the admin API sees it: GET /admin/remote, PUT /admin/remote/endpoint and
// POST/GET/DELETE /admin/remote/pair.
//
// The firmware implements it over RemoteRadio; hub-sim uses SimulatedRemote
// below. Every method is called on the HTTP task and must return quickly:
// RemoteRadio itself is main-loop only, so an implementation hands requests
// to the loop and answers status() from a copy the loop keeps up to date.
// Nothing here may block on, or run on, the RF task, and persisting the
// endpoint (NVS) stays where it is today (off the RF task).
namespace hubbo::api {

// One Harmony Hub radio endpoint: what the remote talks to.
struct RemoteEndpoint {
    uint8_t channel = 0;   // 0..kMaxRemoteChannel
    uint64_t address = 0;  // 40-bit nRF24 address
};

constexpr uint8_t kMaxRemoteChannel = 124;
constexpr uint32_t kRemoteScanTimeoutMs = 120000;

// The console's `radio set` rule (RemoteRadio's RadioEndpoint::isValid):
// channel 0..124; address within 40 bits, not all zeros or all ones, and its
// low byte (sent first) not 0x55/0xAA (looks like the preamble).
bool isValidRemoteEndpoint(const RemoteEndpoint &endpoint);
// A 40-bit address as text: 1-10 hex digits, "0x" optional, either case.
// Parsing only; validity is isValidRemoteEndpoint().
bool parseRemoteAddress(std::string_view text, uint64_t *address);
// The address as 10 uppercase hex digits ("FC5C97550F").
std::string formatRemoteAddress(uint64_t address);

enum class RadioState : uint8_t {
    NotReady,   // radio not started or not responding
    Idle,       // no endpoint known
    Scanning,   // looking for a Hub in pairing mode
    Listening,  // receiving the remote on `endpoint`
};
const char *radioStateName(RadioState state);  // "not_ready", "idle", "scanning", "listening"

// The outcome of the latest scan, kept until the next one starts.
enum class ScanState : uint8_t {
    None,      // no scan since boot
    Scanning,
    Found,     // the endpoint was learned (and saved)
    TimedOut,  // no Hub answered within the timeout
    Stopped,   // cancelled (DELETE), or the radio stopped
};
const char *scanStateName(ScanState state);  // "idle", "scanning", "found", "timeout", "stopped"

struct RemoteRadioStatus {
    RadioState radio = RadioState::NotReady;
    bool hasEndpoint = false;     // an endpoint is known (listening or not)
    RemoteEndpoint endpoint;      // valid when hasEndpoint
    ScanState scan = ScanState::None;
    uint32_t scanRemainingMs = 0; // Scanning only
    uint8_t scanChannel = 0;      // Scanning only: channel being tried
};

enum class RemoteResult : uint8_t {
    Accepted,  // handed to the radio; status() shows the effect shortly
    Invalid,   // setEndpoint(): fails isValidRemoteEndpoint()
    NotReady,  // the radio isn't working
    Busy,      // a scan is running (setEndpoint, startScan)
};

class RemotePort {
public:
    virtual ~RemotePort() = default;

    virtual RemoteRadioStatus status() const = 0;
    // Listen on a known endpoint (and save it), as `radio set` does. Must
    // validate with isValidRemoteEndpoint() itself.
    virtual RemoteResult setEndpoint(const RemoteEndpoint &endpoint) = 0;
    // Start a Hub scan (the Hub's pair button), timing out after `timeoutMs`.
    // A found endpoint replaces the current one and is saved.
    virtual RemoteResult startScan(uint32_t timeoutMs) = 0;
    // Stop a running scan; the previous endpoint (if any) is listened to again.
    // No-op when not scanning.
    virtual void stopScan() = 0;
};

// In-memory radio for hub-sim and tests: Listening once an endpoint is set,
// scans time out unless found() is called. Single task (no locking).
class SimulatedRemote : public RemotePort {
public:
    SimulatedRemote() { status_.radio = RadioState::Idle; }
    RemoteRadioStatus status() const override;
    RemoteResult setEndpoint(const RemoteEndpoint &endpoint) override;
    RemoteResult startScan(uint32_t timeoutMs) override;
    void stopScan() override;

    // Simulation controls.
    void advance(uint32_t elapsedMs);             // scan timeout
    void found(const RemoteEndpoint &endpoint);  // a Hub answered the scan
    bool ready = true;                           // false: NotReady

private:
    RemoteRadioStatus status_;
};

}  // namespace hubbo::api
