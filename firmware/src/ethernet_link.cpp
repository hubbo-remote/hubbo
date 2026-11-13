#include "ethernet_link.h"

#include <Arduino.h>
#include <ETH.h>
#include <esp_mac.h>

#include <algorithm>
#include <cstdio>

#include "board.h"
#include "console.h"

namespace hubbo::fw {

namespace {
constexpr uint32_t kCheckIntervalMs = 250;
}

void EthernetLink::mac(uint8_t out[6]) {
    uint8_t base[6];
    if (esp_read_mac(base, ESP_MAC_ETH) != ESP_OK || esp_derive_local_mac(out, base) != ESP_OK) {
        std::fill(out, out + 6, 0);
    }
}

bool EthernetLink::begin(const char *hostname) {
    Network.setHostname(hostname);
    namespace pins = board::w5500;
    if (!ETH.begin(ETH_PHY_W5500, pins::kPhyAddr, pins::kCs, pins::kInt, pins::kRst, pins::kSpiHost,
                   pins::kSck, pins::kMiso, pins::kMosi)) {
        con.println("eth: W5500 not responding");
        return false;
    }
    started_ = true;
    con.printf("eth: W5500 started, MAC %s\n", ETH.macAddress().c_str());
    return true;
}

EthernetLink::State EthernetLink::poll() const {
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

uint16_t EthernetLink::linkSpeedMbps() const {
    return started_ && ETH.linkUp() ? ETH.linkSpeed() : 0;
}

void EthernetLink::loop(uint32_t nowMs) {
    if (!started_ || nowMs - lastCheckMs_ < kCheckIntervalMs) {
        return;
    }
    lastCheckMs_ = nowMs;
    const State now = poll();
    const IPAddress ip = now == State::Up ? ETH.localIP() : IPAddress();
    if (ip != ip_) {
        ip_ = ip;
        if (now == State::Up) {
            snprintf(ipText_, sizeof(ipText_), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            addressChanged_ = true;
        } else {
            ipText_[0] = '\0';
        }
    }
    if (now == state_ && !(now == State::Up && addressChanged_)) {
        return;
    }
    const State before = state_;
    state_ = now;
    switch (now) {
        case State::NoLink:
            con.println("eth: link down");
            break;
        case State::NoIp:
            con.printf("eth: link up %u Mbps %s duplex, waiting for DHCP\n", linkSpeedMbps(),
                          ETH.fullDuplex() ? "full" : "half");
            break;
        case State::Up:
            con.printf("eth: %s, IP %s\n", before == State::Up ? "new address" : "up", ipText_);
            break;
        case State::Off:
            break;
    }
}

}  // namespace hubbo::fw
