// BleHidAdapter report state: held keys, rollover, consumer replacement,
// release-all and disconnects.
#include <gtest/gtest.h>

#include "fakes.h"
#include "hubbo/integrations/ble_hid.h"

using namespace hubbo;
using namespace hubbo::integrations;

namespace {

class BleHidTest : public ::testing::Test {
protected:
    BleHidTest() {
        hosts.bond("dev_apple_tv", "Apple TV");
        hosts.bond("dev_shield", "SHIELD");
    }

    ExecResult run(BleHidCommand command, const char *key, const char *device = "dev_apple_tv") {
        const int option = bleHidKeyIndex(key);
        EXPECT_GE(option, 0) << key;
        ParamValue params[1] = {ParamValue::ofOption(static_cast<uint16_t>(option))};
        const CommandCatalog catalog = bleHidCatalog();
        const auto index = static_cast<uint16_t>(command);
        return adapter.execute({&catalog.items[index], index, params, 1, device, "host", 1, 0});
    }
    std::vector<std::string> &reports() { return hosts.reports(); }

    sim::SimulatedBleHosts hosts;
    BleHidAdapter adapter{"ble", hosts};
};

}  // namespace

TEST(BleHidCatalog, KnownKeysUseVerifiedUsages) {
    const struct {
        const char *name;
        HidPage page;
        uint16_t usage;
    } known[] = {
        {"ARROW_UP", HidPage::Consumer, 0x42},     {"AC_HOME", HidPage::Consumer, 0x223},
        {"PLAY_PAUSE", HidPage::Consumer, 0xCD},   {"KEY_0", HidPage::Keyboard, 0x27},
        {"FAST_FORWARD", HidPage::Consumer, 0xB3}, {"REWIND", HidPage::Consumer, 0xB4},
        {"STOP", HidPage::Consumer, 0xB7},         {"AC_BACK", HidPage::Consumer, 0x224},
        {"AC_SEARCH", HidPage::Consumer, 0x221},   {"MENU_ESCAPE", HidPage::Consumer, 0x46},
        {"CHANNEL_UP", HidPage::Consumer, 0x9C},   {"CHANNEL_DOWN", HidPage::Consumer, 0x9D},
        {"INFO", HidPage::Consumer, 0x1BD},        {"SLEEP", HidPage::Consumer, 0x34},
        {"KEYPAD_ENTER", HidPage::Keyboard, 0x58}, {"BACKSPACE", HidPage::Keyboard, 0x2A},
        {"ESCAPE", HidPage::Keyboard, 0x29},
    };
    for (const auto &k : known) {
        const HidKey &key = bleHidKey(bleHidKeyIndex(k.name));
        EXPECT_EQ(key.page, k.page) << k.name;
        EXPECT_EQ(key.usage, k.usage) << k.name;
    }
    EXPECT_EQ(bleHidKeyIndex("NOPE"), -1);
    EXPECT_EQ(bleHidKeyIndex(nullptr), -1);
    const CommandCatalog c = bleHidCatalog();
    EXPECT_EQ(c.find("key_down"), 0);
    EXPECT_EQ(c.find("key_up"), 1);
    EXPECT_EQ(c.find("key_tap"), 2);
    EXPECT_EQ(c.items[0].params[0].optionCount, bleHidKeyCount());
}

TEST_F(BleHidTest, ConsumerKeyDownUp) {
    EXPECT_EQ(run(BleHidCommand::KeyDown, "ARROW_UP").status, ExecStatus::Accepted);
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0x42);
    run(BleHidCommand::KeyDown, "ARROW_UP");  // repeat: no duplicate report
    run(BleHidCommand::KeyUp, "ARROW_UP");
    EXPECT_EQ(reports(), (std::vector<std::string>{"consumer 0x0042", "consumer 0x0000"}));
}

TEST_F(BleHidTest, NewConsumerKeyReplacesTheHeldOne) {
    run(BleHidCommand::KeyDown, "ARROW_UP");
    run(BleHidCommand::KeyDown, "ARROW_LEFT");
    run(BleHidCommand::KeyUp, "ARROW_UP");  // no longer held: no-op
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0x44);
    EXPECT_EQ(reports().size(), 2u);
}

TEST_F(BleHidTest, KeyboardKeysAndRollover) {
    const char *keys[] = {"KEY_1", "KEY_2", "KEY_3", "KEY_4", "KEY_5", "KEY_6"};
    for (const char *k : keys) EXPECT_EQ(run(BleHidCommand::KeyDown, k).status, ExecStatus::Accepted);
    EXPECT_EQ(reports().back(), "keyboard 00 00 1e 1f 20 21 22 23");
    const ExecResult seventh = run(BleHidCommand::KeyDown, "KEY_7");
    EXPECT_EQ(seventh.status, ExecStatus::Failed);
    EXPECT_EQ(seventh.error, ErrorCode::CapacityExceeded);
    run(BleHidCommand::KeyUp, "KEY_2");
    EXPECT_EQ(reports().back(), "keyboard 00 00 1e 20 21 22 23 00");
    EXPECT_EQ(adapter.heldKeyboardKeys("dev_apple_tv"), 5u);
}

TEST_F(BleHidTest, TapIsDownThenUp) {
    run(BleHidCommand::KeyTap, "KEY_I");
    EXPECT_EQ(reports(), (std::vector<std::string>{"keyboard 00 00 0c 00 00 00 00 00",
                                                   "keyboard 00 00 00 00 00 00 00 00"}));
}

TEST_F(BleHidTest, ReleaseAllClearsEverything) {
    run(BleHidCommand::KeyDown, "KEY_1");
    run(BleHidCommand::KeyDown, "SELECT");
    adapter.releaseAll(0);
    EXPECT_EQ(adapter.heldKeyboardKeys("dev_apple_tv"), 0u);
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0);
    const auto n = reports().size();
    EXPECT_EQ(reports()[n - 2], "keyboard 00 00 00 00 00 00 00 00");
    EXPECT_EQ(reports()[n - 1], "consumer 0x0000");
    adapter.releaseAll(0);  // nothing held: nothing sent
    EXPECT_EQ(reports().size(), n);
}

TEST_F(BleHidTest, DisconnectedFailsAndForgetsHeldKeys) {
    run(BleHidCommand::KeyDown, "SELECT");
    hosts.setConnected("dev_apple_tv", false);
    const ExecResult r = run(BleHidCommand::KeyUp, "SELECT");
    EXPECT_EQ(r.status, ExecStatus::Failed);
    EXPECT_EQ(r.error, ErrorCode::AdapterUnavailable);
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0);
    hosts.setConnected("dev_apple_tv", true);
    adapter.releaseAll(0);
    EXPECT_EQ(reports().size(), 1u);  // nothing stale re-sent
}

// Each device is its own host: a key goes only to the named device's host,
// held state is per host, and one host dropping leaves the others alone.
TEST_F(BleHidTest, EachDeviceIsItsOwnHost) {
    run(BleHidCommand::KeyDown, "ARROW_UP", "dev_apple_tv");
    run(BleHidCommand::KeyDown, "PLAY_PAUSE", "dev_shield");
    EXPECT_EQ(hosts.reportDevices(), (std::vector<std::string>{"dev_apple_tv", "dev_shield"}));
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0x42);
    EXPECT_EQ(adapter.heldConsumer("dev_shield"), 0xCD);

    hosts.setConnected("dev_shield", false);
    const ExecResult r = run(BleHidCommand::KeyTap, "SELECT", "dev_shield");
    EXPECT_EQ(r.error, ErrorCode::AdapterUnavailable);  // nothing is sent anywhere else
    EXPECT_EQ(reports().size(), 2u);
    EXPECT_EQ(adapter.heldConsumer("dev_shield"), 0);
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0x42);

    EXPECT_EQ(run(BleHidCommand::KeyTap, "SELECT", "dev_fire_tv").error, ErrorCode::AdapterUnavailable);  // not paired

    adapter.releaseAll(0);  // an activity change: every host's keys
    EXPECT_EQ(reports().back(), "consumer 0x0000");
    EXPECT_EQ(hosts.reportDevices().back(), "dev_apple_tv");
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0);
}

TEST_F(BleHidTest, NewConsumerKeyTaps) {
    run(BleHidCommand::KeyTap, "FAST_FORWARD");
    EXPECT_EQ(reports(), (std::vector<std::string>{"consumer 0x00b3", "consumer 0x0000"}));
}

TEST_F(BleHidTest, PollForgetsHeldKeysWhenTheLinkDrops) {
    struct NoSink : CompletionSink {
        void complete(Ticket, ExecResult, uint32_t) override {}
    } sink;
    run(BleHidCommand::KeyDown, "ARROW_UP");
    hosts.setConnected("dev_apple_tv", false);
    adapter.poll(0, sink);
    hosts.setConnected("dev_apple_tv", true);    // reconnects before any further command
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0);
    run(BleHidCommand::KeyDown, "KEY_1");
    EXPECT_EQ(reports().back(), "keyboard 00 00 1e 00 00 00 00 00");  // no stale consumer key
    EXPECT_EQ(reports().size(), 2u);
}
