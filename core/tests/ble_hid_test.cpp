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
    ExecResult run(BleHidCommand command, const char *key) {
        const int option = bleHidKeyIndex(key);
        EXPECT_GE(option, 0) << key;
        ParamValue params[1] = {ParamValue::ofOption(static_cast<uint16_t>(option))};
        const CommandCatalog catalog = bleHidCatalog();
        const auto index = static_cast<uint16_t>(command);
        return adapter.execute({&catalog.items[index], index, params, 1, "dev_apple_tv", "host", 1, 0});
    }
    std::vector<std::string> &reports() { return hid.reports(); }

    sim::RecordingHidTransport hid;
    BleHidAdapter adapter{"ble", hid};
};

}  // namespace

TEST(BleHidCatalog, KnownKeysUseVerifiedUsages) {
    EXPECT_EQ(bleHidKey(bleHidKeyIndex("ARROW_UP")).usage, 0x42);
    EXPECT_EQ(bleHidKey(bleHidKeyIndex("AC_HOME")).usage, 0x223);
    EXPECT_EQ(bleHidKey(bleHidKeyIndex("PLAY_PAUSE")).usage, 0xCD);
    EXPECT_EQ(bleHidKey(bleHidKeyIndex("KEY_0")).page, HidPage::Keyboard);
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
    EXPECT_EQ(adapter.heldConsumer(), 0x42);
    run(BleHidCommand::KeyDown, "ARROW_UP");  // repeat: no duplicate report
    run(BleHidCommand::KeyUp, "ARROW_UP");
    EXPECT_EQ(reports(), (std::vector<std::string>{"consumer 0x0042", "consumer 0x0000"}));
}

TEST_F(BleHidTest, NewConsumerKeyReplacesTheHeldOne) {
    run(BleHidCommand::KeyDown, "ARROW_UP");
    run(BleHidCommand::KeyDown, "ARROW_LEFT");
    run(BleHidCommand::KeyUp, "ARROW_UP");  // no longer held: no-op
    EXPECT_EQ(adapter.heldConsumer(), 0x44);
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
    EXPECT_EQ(adapter.heldKeyboardKeys(), 5u);
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
    EXPECT_EQ(adapter.heldKeyboardKeys(), 0u);
    EXPECT_EQ(adapter.heldConsumer(), 0);
    const auto n = reports().size();
    EXPECT_EQ(reports()[n - 2], "keyboard 00 00 00 00 00 00 00 00");
    EXPECT_EQ(reports()[n - 1], "consumer 0x0000");
    adapter.releaseAll(0);  // nothing held: nothing sent
    EXPECT_EQ(reports().size(), n);
}

TEST_F(BleHidTest, DisconnectedFailsAndForgetsHeldKeys) {
    run(BleHidCommand::KeyDown, "SELECT");
    hid.setConnected(false);
    const ExecResult r = run(BleHidCommand::KeyUp, "SELECT");
    EXPECT_EQ(r.status, ExecStatus::Failed);
    EXPECT_EQ(r.error, ErrorCode::AdapterUnavailable);
    EXPECT_EQ(adapter.heldConsumer(), 0);
    hid.setConnected(true);
    adapter.releaseAll(0);
    EXPECT_EQ(reports().size(), 1u);  // nothing stale re-sent
}

TEST_F(BleHidTest, PollForgetsHeldKeysWhenTheLinkDrops) {
    struct NoSink : CompletionSink {
        void complete(Ticket, ExecResult, uint32_t) override {}
    } sink;
    run(BleHidCommand::KeyDown, "ARROW_UP");
    hid.setConnected(false);
    adapter.poll(0, sink);
    hid.setConnected(true);    // reconnects before any further command
    EXPECT_EQ(adapter.heldConsumer(), 0);
    run(BleHidCommand::KeyDown, "KEY_1");
    EXPECT_EQ(reports().back(), "keyboard 00 00 1e 00 00 00 00 00");  // no stale consumer key
    EXPECT_EQ(reports().size(), 2u);
}
