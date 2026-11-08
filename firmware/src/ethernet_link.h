#pragma once

// Onboard W5500 Ethernet with DHCP. Non-blocking: begin() starts the
// driver and returns; link and address changes are logged from loop().
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

    State state() const;
    static const char *stateName(State state);
    IPAddress ip() const;
    uint16_t linkSpeedMbps() const;
    bool fullDuplex() const;

    // As of the last loop(): link up with an address. Cheap (no driver call).
    bool up() const { return lastState_ == State::Up; }

private:
    bool started_ = false;
    State lastState_ = State::Off;
    uint32_t lastCheckMs_ = 0;
};

}  // namespace hubbo::fw
