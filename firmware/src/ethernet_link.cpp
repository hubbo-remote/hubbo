#include "ethernet_link.h"

#include <Arduino.h>
#include <ETH.h>

#include "board.h"

namespace hubbo::fw {

namespace {
constexpr uint32_t kCheckIntervalMs = 250;
}

bool EthernetLink::begin(const char *hostname) {
    Network.setHostname(hostname);
    namespace pins = board::w5500;
    if (!ETH.begin(ETH_PHY_W5500, pins::kPhyAddr, pins::kCs, pins::kInt, pins::kRst, pins::kSpiHost,
                   pins::kSck, pins::kMiso, pins::kMosi)) {
        Serial.println("eth: W5500 not responding");
        return false;
    }
    started_ = true;
    Serial.printf("eth: W5500 started, MAC %s\n", ETH.macAddress().c_str());
    return true;
}

EthernetLink::State EthernetLink::state() const {
    if (!started_) {
        return State::Off;
    }
    if (!ETH.linkUp()) {
        return State::NoLink;
    }
    return ETH.hasIP() ? State::Up : State::NoIp;
}

const char *EthernetLink::stateName(State state) {
    switch (state) {
        case State::Off: return "off";
        case State::NoLink: return "no_link";
        case State::NoIp: return "no_ip";
        case State::Up: return "up";
    }
    return "off";
}

IPAddress EthernetLink::ip() const {
    return started_ ? ETH.localIP() : IPAddress();
}

uint16_t EthernetLink::linkSpeedMbps() const {
    return started_ && ETH.linkUp() ? ETH.linkSpeed() : 0;
}

bool EthernetLink::fullDuplex() const {
    return started_ && ETH.linkUp() && ETH.fullDuplex();
}

void EthernetLink::loop(uint32_t nowMs) {
    if (!started_ || nowMs - lastCheckMs_ < kCheckIntervalMs) {
        return;
    }
    lastCheckMs_ = nowMs;
    const State now = state();
    if (now == lastState_) {
        return;
    }
    lastState_ = now;
    switch (now) {
        case State::NoLink:
            Serial.println("eth: link down");
            break;
        case State::NoIp:
            Serial.printf("eth: link up %u Mbps %s duplex, waiting for DHCP\n", linkSpeedMbps(),
                          fullDuplex() ? "full" : "half");
            break;
        case State::Up:
            Serial.printf("eth: up, IP %s\n", ETH.localIP().toString().c_str());
            break;
        case State::Off:
            break;
    }
}

}  // namespace hubbo::fw
