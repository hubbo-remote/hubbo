// BleHidAdapter: the catalog from a device's profile, report state (held
// keys, rollover, consumer replacement, release-all, disconnects), timed
// presses and waiting for a connection.
#include <gtest/gtest.h>

#include <set>

#include "fakes.h"
#include "fixtures.h"
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

    // A key from the All keys table.
    ExecResult run(BleHidCommand command, const char *key, const char *device = "dev_apple_tv") {
        const int option = bleHidKeyIndex(key);
        EXPECT_GE(option, 0) << key;
        return exec(command, {ParamValue::ofOption(static_cast<uint16_t>(option))}, 1, device);
    }
    // Any command of the Apple TV profile's catalog, with params in spec
    // order; `present` marks which were given.
    ExecResult exec(BleHidCommand command, std::vector<ParamValue> params, uint8_t present,
                    const char *device = "dev_apple_tv", uint32_t nowMs = 0, Ticket ticket = 1,
                    bool direct = false) {
        const CommandCatalog catalog = adapter.commandsFor(sim::bleDevice(device, "Device", "streamer"));
        const auto index = static_cast<uint16_t>(command);
        const CommandSpec &spec = catalog.items[index];
        while (params.size() < spec.paramCount) params.push_back(spec.params[params.size()].defaultValue);
        return adapter.execute({&spec, index, params.data(), present, device, "host", ticket, nowMs, direct});
    }
    // press {command} by the profile's command name.
    ExecResult press(BleHidCommand command, const char *name, std::optional<int32_t> holdMs = std::nullopt,
                     uint32_t nowMs = 0, Ticket ticket = 1, bool direct = false) {
        const CommandCatalog catalog = adapter.commandsFor(sim::bleDevice("dev_apple_tv", "Apple TV", "streamer"));
        const ParamSpec &names = catalog.items[0].params[0];
        int option = -1;
        for (uint16_t i = 0; i < names.optionCount; i++) {
            if (std::string(names.options[i]) == name) option = i;
        }
        EXPECT_GE(option, 0) << name;
        std::vector<ParamValue> params = {ParamValue::ofOption(static_cast<uint16_t>(option))};
        if (holdMs) params.push_back(ParamValue::ofInt(*holdMs));
        return exec(command, params, holdMs ? 0x3 : 0x1, "dev_apple_tv", nowMs, ticket, direct);
    }
    std::vector<std::string> &reports() { return hosts.reports(); }

    struct Sink : CompletionSink {
        std::vector<std::pair<Ticket, ExecResult>> done;
        void complete(Ticket t, ExecResult r, uint32_t) override { done.push_back({t, r}); }
    } sink;
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
}

// All keys: every keyboard usage the report map carries, once.
TEST(BleHidCatalog, EveryKeyboardUsageHasOneKey) {
    std::set<uint16_t> keyboard;
    for (size_t i = 0; i < bleHidKeyCount(); i++) {
        const HidKey &key = bleHidKey(i);
        EXPECT_EQ(bleHidKeyIndex(key.name), static_cast<int>(i)) << key.name;  // unique names
        if (key.page != HidPage::Keyboard) continue;
        EXPECT_TRUE(keyboard.insert(key.usage).second) << key.name;
    }
    EXPECT_EQ(keyboard.size(), limits::kHidKeyboardUsageMax - limits::kHidKeyboardUsageMin + 1u);
    EXPECT_EQ(*keyboard.begin(), limits::kHidKeyboardUsageMin);
    EXPECT_EQ(*keyboard.rbegin(), limits::kHidKeyboardUsageMax);
}

TEST_F(BleHidTest, TheCatalogFollowsTheProfile) {
    const DeviceSpec appleTv = sim::bleDevice("dev_apple_tv", "Apple TV", "streamer");
    const CommandCatalog c = adapter.commandsFor(appleTv);
    ASSERT_EQ(c.count, 10);
    const char *ids[] = {"press",     "press_down", "press_up", "key_tap",  "key_down",
                         "key_up",    "usage_tap",  "usage_down", "usage_up", "wait_connected"};
    const char *groups[] = {"remote", "remote", "remote", "keys", "keys", "keys", "custom", "custom", "custom", nullptr};
    for (uint16_t i = 0; i < c.count; i++) {
        EXPECT_STREQ(c.items[i].id, ids[i]);
        if (groups[i] == nullptr) {
            EXPECT_EQ(c.items[i].group, nullptr);
        } else {
            EXPECT_STREQ(c.items[i].group, groups[i]);
        }
    }
    EXPECT_EQ(c.items[1].effect, CommandEffect::Hold);
    EXPECT_STREQ(c.items[1].releasedBy, "press_up");
    EXPECT_EQ(c.items[7].effect, CommandEffect::Hold);
    EXPECT_EQ(c.items[9].effect, CommandEffect::Wait);
    // The `command` options are the profile's names, in order.
    const ParamSpec &command = c.items[0].params[0];
    ASSERT_EQ(command.optionCount, appleTv.profile->commands.size());
    EXPECT_STREQ(command.options[0], appleTv.profile->commands[0].name.c_str());
    EXPECT_EQ(c.items[3].params[0].optionCount, bleHidKeyCount());
    // Unchanged commands: the same storage; changed ones: new storage, and
    // the old catalog stays valid.
    EXPECT_EQ(adapter.commandsFor(appleTv).items, c.items);
    DeviceSpec changed = appleTv;
    changed.profile->commands.pop_back();
    const CommandCatalog other = adapter.commandsFor(changed);
    EXPECT_NE(other.items, c.items);
    EXPECT_EQ(other.items[0].params[0].optionCount, command.optionCount - 1);
    EXPECT_STREQ(c.items[0].params[0].options[0], appleTv.profile->commands[0].name.c_str());
    // No profile: no commands.
    DeviceSpec bare = appleTv;
    bare.profile.reset();
    EXPECT_EQ(adapter.commandsFor(bare).count, 0);
}

TEST_F(BleHidTest, PressSendsTheProfilesUsage) {
    EXPECT_EQ(press(BleHidCommand::Press, "Back").status, ExecStatus::Accepted);  // Apple TV: 0x223
    EXPECT_EQ(reports(), (std::vector<std::string>{"consumer 0x0223", "consumer 0x0000"}));
    press(BleHidCommand::PressDown, "Select");
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0x41);
    press(BleHidCommand::PressUp, "Select");
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0);
}

TEST_F(BleHidTest, ACommandsHoldIsATimedPress) {
    // ControlCenter: Home held 1 s. Pending until poll() releases it.
    ASSERT_EQ(press(BleHidCommand::Press, "ControlCenter", std::nullopt, 100, 7).status, ExecStatus::Pending);
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0x40);
    adapter.poll(1099, sink);
    EXPECT_TRUE(sink.done.empty());
    adapter.poll(1100, sink);
    ASSERT_EQ(sink.done.size(), 1u);
    EXPECT_EQ(sink.done[0].first, 7u);
    EXPECT_EQ(sink.done[0].second.status, ExecStatus::Accepted);
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0);
    EXPECT_EQ(reports(), (std::vector<std::string>{"consumer 0x0040", "consumer 0x0000"}));
}

TEST_F(BleHidTest, AStepsHoldOverridesTheCommands) {
    ASSERT_EQ(press(BleHidCommand::Press, "Home", 2000, 0, 3).status, ExecStatus::Pending);
    adapter.poll(1999, sink);
    EXPECT_TRUE(sink.done.empty());
    adapter.poll(2000, sink);
    ASSERT_EQ(sink.done.size(), 1u);
}

// releaseAll() is for keys a binding holds; a timed press in a sequence (the
// Apple TV's power-off holds Home 2 s) keeps its key until its time.
TEST_F(BleHidTest, ReleaseAllLeavesATimedPressAlone) {
    ASSERT_EQ(press(BleHidCommand::Press, "Home", 2000, 0, 3).status, ExecStatus::Pending);
    run(BleHidCommand::KeyDown, "KEY_A");  // a binding's held key
    adapter.releaseAll(500);
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0x40);
    EXPECT_EQ(adapter.heldKeyboardKeys("dev_apple_tv"), 0u);
    adapter.poll(1999, sink);
    EXPECT_TRUE(sink.done.empty());
    adapter.poll(2000, sink);
    ASSERT_EQ(sink.done.size(), 1u);
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0);
}

// Cancelling the sequence ends its timed press and its wait at once, with no
// completion, and frees their slots.
TEST_F(BleHidTest, CancelEndsATimedPressOrAWait) {
    ASSERT_EQ(press(BleHidCommand::Press, "Home", 5000, 0, 4).status, ExecStatus::Pending);
    adapter.cancel(4, 100);
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0);
    adapter.cancel(99, 100);  // unknown: ignored
    hosts.setConnected("dev_apple_tv", false);
    for (Ticket t = 10; t < 18; t++) {
        ASSERT_EQ(exec(BleHidCommand::WaitConnected, {}, 0, "dev_apple_tv", 0, t).status, ExecStatus::Pending) << t;
    }
    EXPECT_EQ(exec(BleHidCommand::WaitConnected, {}, 0, "dev_apple_tv", 0, 18).error, ErrorCode::CapacityExceeded);
    adapter.cancel(10, 100);
    EXPECT_EQ(exec(BleHidCommand::WaitConnected, {}, 0, "dev_apple_tv", 0, 18).status, ExecStatus::Pending);
    adapter.poll(60000, sink);
    for (const auto &[ticket, result] : sink.done) EXPECT_NE(ticket, 10u);
    EXPECT_EQ(sink.done.size(), 8u);
}

// A direct execute answers at key-down; the release happens by itself.
TEST_F(BleHidTest, ADirectTimedPressAnswersAtKeyDown) {
    ASSERT_EQ(press(BleHidCommand::Press, "Home", 2000, 0, 5, /*direct=*/true).status, ExecStatus::Accepted);
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0x40);
    adapter.poll(2000, sink);
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0);
    EXPECT_TRUE(sink.done.empty());
    EXPECT_EQ(reports(), (std::vector<std::string>{"consumer 0x0040", "consumer 0x0000"}));
}

TEST_F(BleHidTest, ACustomUsage) {
    EXPECT_EQ(exec(BleHidCommand::UsageTap, {ParamValue::ofInt(0x0224)}, 1).status, ExecStatus::Accepted);
    EXPECT_EQ(reports(), (std::vector<std::string>{"consumer 0x0224", "consumer 0x0000"}));
    exec(BleHidCommand::UsageDown, {ParamValue::ofInt(0x0030)}, 1);
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0x30);
    exec(BleHidCommand::UsageUp, {ParamValue::ofInt(0x0030)}, 1);
    EXPECT_EQ(adapter.heldConsumer("dev_apple_tv"), 0);
}

TEST_F(BleHidTest, WaitConnected) {
    // Connected: done at once.
    EXPECT_EQ(exec(BleHidCommand::WaitConnected, {}, 0).status, ExecStatus::Accepted);
    // Not yet: Pending until the host connects.
    hosts.setConnected("dev_apple_tv", false);
    ASSERT_EQ(exec(BleHidCommand::WaitConnected, {}, 0, "dev_apple_tv", 0, 5).status, ExecStatus::Pending);
    adapter.poll(1000, sink);
    EXPECT_TRUE(sink.done.empty());
    hosts.setConnected("dev_apple_tv", true);
    adapter.poll(1200, sink);
    ASSERT_EQ(sink.done.size(), 1u);
    EXPECT_EQ(sink.done[0].second.status, ExecStatus::Accepted);
    EXPECT_EQ(sink.done[0].second.error, ErrorCode::Ok);
    // Never connects: accepted after the timeout (default 15 s), with why.
    hosts.setConnected("dev_apple_tv", false);
    ASSERT_EQ(exec(BleHidCommand::WaitConnected, {}, 0, "dev_apple_tv", 2000, 6).status, ExecStatus::Pending);
    adapter.poll(16999, sink);
    EXPECT_EQ(sink.done.size(), 1u);
    adapter.poll(17000, sink);
    ASSERT_EQ(sink.done.size(), 2u);
    EXPECT_EQ(sink.done[1].second.status, ExecStatus::Accepted);
    EXPECT_EQ(sink.done[1].second.error, ErrorCode::AdapterUnavailable);
    // A shorter timeout of its own.
    ASSERT_EQ(exec(BleHidCommand::WaitConnected, {ParamValue::ofInt(1000)}, 1, "dev_apple_tv", 20000, 8).status,
              ExecStatus::Pending);
    adapter.poll(21000, sink);
    EXPECT_EQ(sink.done.size(), 3u);
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
