// The shared ble_hid state mapping (integrations/ble_state.h) that the
// firmware and hub-sim both report through.
#include <gtest/gtest.h>

#include <string>

#include "hubbo/integrations/ble_state.h"

using namespace hubbo;
using namespace hubbo::integrations;
using api::BlePairState;

namespace {

IntegrationSpec ble(bool enabled = true) {
    IntegrationSpec spec;
    spec.id = "ble";
    spec.type = kBleHidType;
    spec.enabled = enabled;
    return spec;
}

TEST(BleState, IntegrationStateFollowsTheBondThenTheLink) {
    const IntegrationSpec spec = ble();
    EXPECT_STREQ(bleIntegrationState(spec, BlePairState::NotReady, false), "error");
    EXPECT_STREQ(bleIntegrationState(spec, BlePairState::Idle, false), "not_configured");
    EXPECT_STREQ(bleIntegrationState(spec, BlePairState::Failed, false), "not_configured");
    EXPECT_STREQ(bleIntegrationState(spec, BlePairState::Pairing, false), "connecting");
    EXPECT_STREQ(bleIntegrationState(spec, BlePairState::Paired, false), "disconnected");
    EXPECT_STREQ(bleIntegrationState(spec, BlePairState::Paired, true), "connected");
    // The link only counts with a bond.
    EXPECT_STREQ(bleIntegrationState(spec, BlePairState::Idle, true), "not_configured");
}

TEST(BleState, DisabledComesFirst) {
    EXPECT_STREQ(bleIntegrationState(ble(false), BlePairState::Paired, true), "disabled");
    EXPECT_STREQ(bleIntegrationState(ble(false), BlePairState::NotReady, false), "disabled");
}

TEST(BleState, DetailNamesThePeer) {
    EXPECT_EQ(bleIntegrationDetail(BlePairState::NotReady, false, ""), "Bluetooth isn't working");
    EXPECT_EQ(bleIntegrationDetail(BlePairState::Idle, false, ""), "Not paired");
    EXPECT_EQ(bleIntegrationDetail(BlePairState::Failed, false, ""), "Not paired");
    EXPECT_EQ(bleIntegrationDetail(BlePairState::Pairing, false, ""), "Ready to pair");
    EXPECT_EQ(bleIntegrationDetail(BlePairState::Paired, false, "Family Room"), "Waiting for Family Room");
    EXPECT_EQ(bleIntegrationDetail(BlePairState::Paired, true, "Family Room"), "Connected to Family Room");
    EXPECT_EQ(bleIntegrationDetail(BlePairState::Paired, true, ""), "Connected to your device");
}

TEST(BleState, DeviceIsAvailableOnlyWhenBondedAndConnected) {
    EXPECT_STREQ(bleDeviceAvailability(BlePairState::Paired, true), "available");
    EXPECT_STREQ(bleDeviceAvailability(BlePairState::Paired, false), "unavailable");
    EXPECT_STREQ(bleDeviceAvailability(BlePairState::Pairing, true), "unavailable");
    EXPECT_STREQ(bleDeviceAvailability(BlePairState::Idle, true), "unavailable");
    EXPECT_STREQ(bleDeviceAvailability(BlePairState::NotReady, false), "unavailable");
}

}  // namespace
