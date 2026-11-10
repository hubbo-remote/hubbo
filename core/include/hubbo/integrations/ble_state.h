#pragma once

#include <string>
#include <string_view>

#include "hubbo/api/ble_pairing_port.h"
#include "hubbo/domain/config.h"

// The ble_hid integration as api::Runtime reports it, from the bond (the
// pairing port's state) and the host link. The firmware and hub-sim share
// these, so the board and the sim always say the same thing.
//
// The board's BleHid states map onto the inputs as:
//   Off -> NotReady; NotPaired -> Idle or Failed; Pairing -> Pairing;
//   Waiting -> Paired, link down; Connected -> Paired, link up.
// `hostConnected` only counts when Paired.
namespace hubbo::integrations {

// IntegrationState: "disabled" first, then "error" (NotReady), "connecting"
// (Pairing), "connected" / "disconnected" (Paired), else "not_configured".
const char *bleIntegrationState(const IntegrationSpec &integration, api::BlePairState pairing, bool hostConnected);

// The integration's detail line, e.g. "Connected to Family Room".
std::string bleIntegrationDetail(api::BlePairState pairing, bool hostConnected, std::string_view peerName);

// A ble_hid device's availability: "available" only while Paired and the
// host is connected, else "unavailable".
const char *bleDeviceAvailability(api::BlePairState pairing, bool hostConnected);

}  // namespace hubbo::integrations
