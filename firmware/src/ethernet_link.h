#pragma once

// Onboard W5500 Ethernet with DHCP. Non-blocking: begin() starts the
// driver and returns; loop() polls link and address every 250 ms, logs
// changes and caches them, so the getters below cost no driver call.
//
// Same arduino-esp32 3.x ETH call as Harmonizer's src/network_link.cpp
// (soaresbrun0/Harmonizer, AGPL-3.0), without its blocking wait for DHCP.
//
// All methods are for the main loop only.

#include <IPAddress.h>
#include <cstdint>

namespace hubbo::fw {

class EthernetLink {
public:
    enum class State : uint8_t {
        Off,     // driver not started or W5500 not responding
        NoLink,  // cable unplugged / no carrier
        NoIp,    // link up, waiting for DHCP
        Up,      // link up with an address
    };

    bool begin(const char *hostname);
    void loop(uint32_t nowMs);

    // As of the last loop().
    State state() const { return state_; }
    bool up() const { return state_ == State::Up; }
    IPAddress ip() const { return ip_; }
    const char *ipText() const { return ipText_; }  // "" when not up
    static const char *stateName(State state);
    uint16_t linkSpeedMbps() const;  // driver call

    // True once after each new address (DHCP lease, renewal to a new IP).
    bool takeAddressChange() {
        const bool changed = addressChanged_;
        addressChanged_ = false;
        return changed;
    }

private:
    State poll() const;

    bool started_ = false;
    State state_ = State::Off;
    IPAddress ip_;
    char ipText_[16] = {};
    bool addressChanged_ = false;
    uint32_t lastCheckMs_ = 0;
};

}  // namespace hubbo::fw
