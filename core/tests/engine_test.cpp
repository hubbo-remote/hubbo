// Activity lifecycle and the hot path, driven through the real RemoteInput
// with the Watch Apple TV / Play Xbox fixtures (SimHub).
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "fixtures.h"
#include "hub.h"

using namespace hubbo;
using namespace hubbo::sim;
using remote::Button;

namespace {

class EngineTest : public ::testing::Test {
protected:
    EngineTest() {
        const ValidationReport r = hub.load(fixtureConfig());
        EXPECT_TRUE(r.ok());
    }

    // Command lines since the last call, without the " -> status" suffix.
    std::vector<std::string> take() {
        std::vector<std::string> out;
        for (; seen < hub.commands().size(); seen++) {
            const std::string &line = hub.commands()[seen].text;
            out.push_back(line.substr(0, line.find(" -> ")));
        }
        return out;
    }

    bool anyContains(const std::vector<std::string> &lines, const std::string &needle) {
        return std::any_of(lines.begin(), lines.end(),
                           [&](const std::string &l) { return l.find(needle) != std::string::npos; });
    }

    SimHub hub;
    size_t seen = 0;
};

using Lines = std::vector<std::string>;

}  // namespace

TEST_F(EngineTest, ActivateRunsOnlyTheTargetStartup) {
    ASSERT_EQ(hub.activate("act_watch_apple_tv"), ErrorCode::Ok);
    EXPECT_STREQ(hub.engine().activeActivityId(), "act_watch_apple_tv");
    EXPECT_EQ(take(), (Lines{"ha_main dev_lg_tv media_player.turn_on", "ha_main dev_denon media_player.turn_on",
                             "system - wait duration_ms=1000"}));
    hub.advance(1000);
    EXPECT_EQ(take(), (Lines{"ha_main dev_lg_tv media_player.select_source source=\"HDMI 1\"",
                             "ha_main dev_denon media_player.select_source source=\"Media Player\""}));
}

TEST_F(EngineTest, SwitchingNeverRunsTheOldShutdown) {
    hub.activate("act_play_xbox");
    hub.advance(3000);
    take();
    hub.activate("act_watch_apple_tv");
    hub.advance(3000);
    const Lines lines = take();
    EXPECT_FALSE(anyContains(lines, "turn_off"));  // no Xbox (or anything) off
    EXPECT_FALSE(anyContains(lines, "dev_xbox"));
    EXPECT_TRUE(anyContains(lines, "source=\"HDMI 1\""));
}

TEST_F(EngineTest, OffRunsOnlyTheCurrentShutdown) {
    hub.activate("act_play_xbox");
    hub.advance(3000);
    hub.activate("act_watch_apple_tv");
    hub.advance(3000);
    take();
    ASSERT_EQ(hub.off(), ErrorCode::Ok);
    EXPECT_FALSE(hub.engine().hasActiveActivity());
    EXPECT_EQ(take(), (Lines{"ha_main dev_lg_tv media_player.turn_off", "ha_main dev_denon media_player.turn_off"}));
    EXPECT_EQ(hub.off(), ErrorCode::NoActiveActivity);
    EXPECT_TRUE(take().empty());
}

TEST_F(EngineTest, UnknownActivityChangesNothing) {
    hub.activate("act_watch_apple_tv");
    take();
    EXPECT_EQ(hub.activate("netflix"), ErrorCode::UnknownActivity);
    EXPECT_STREQ(hub.engine().activeActivityId(), "act_watch_apple_tv");
    EXPECT_TRUE(take().empty());
}

TEST_F(EngineTest, SwitchCancelsTheOldStartupSoItCannotResume) {
    hub.activate("act_play_xbox");  // waits 2000 ms before selecting inputs
    hub.advance(500);
    hub.activate("act_watch_apple_tv");
    hub.advance(5000);
    const Lines lines = take();
    EXPECT_FALSE(anyContains(lines, "HDMI 2"));
    EXPECT_FALSE(anyContains(lines, "source=Game"));
    const uint32_t gen = hub.engine().generation();
    EXPECT_EQ(hub.engine().executor().generation(), gen);
}

TEST_F(EngineTest, RepeatedSwitchesAreDeterministic) {
    auto runOnce = [] {
        SimHub h;
        h.load(fixtureConfig());
        for (int i = 0; i < 20; i++) {
            h.activate(i % 2 ? "act_watch_apple_tv" : "act_play_xbox");
            h.advance(static_cast<uint32_t>(300 * (i % 7)));
        }
        h.off();
        h.advance(5000);
        std::string all;
        for (const auto &c : h.commands()) all += std::to_string(c.atMs) + " " + c.text + "\n";
        return all;
    };
    EXPECT_EQ(runOnce(), runOnce());
}

TEST_F(EngineTest, ButtonsDispatchThroughTheActiveTable) {
    hub.activate("act_watch_apple_tv");
    hub.advance(1000);
    take();
    hub.press(Button::Up);
    EXPECT_EQ(take(), (Lines{"ble dev_apple_tv key_down key=ARROW_UP"}));  // immediate, no time passes
    hub.advance(400);
    EXPECT_TRUE(take().empty());  // held: tvOS repeats, we don't
    hub.release(Button::Up);
    EXPECT_EQ(take(), (Lines{"ble dev_apple_tv key_up key=ARROW_UP"}));
    EXPECT_EQ(hub.ble().heldConsumer(), 0);
}

TEST_F(EngineTest, UnboundTriggersAreNoOps) {
    hub.activate("act_play_xbox");
    take();
    const auto unboundBefore = hub.engine().stats().unbound;
    hub.tap(Button::Up);  // Play Xbox doesn't bind arrows
    hub.tap(Button::Number5);
    EXPECT_TRUE(take().empty());
    EXPECT_GT(hub.engine().stats().unbound, unboundBefore);
}

TEST_F(EngineTest, NothingIsBoundBeforeConfigIsLoaded) {
    SimHub empty;
    empty.press(Button::Up);
    empty.release(Button::Up);
    EXPECT_TRUE(empty.commands().empty());
    EXPECT_EQ(empty.activate("act_watch_apple_tv"), ErrorCode::UnknownActivity);
}

TEST_F(EngineTest, GlobalBindingsWorkWithNoActivity) {
    hub.tap(Button::Tv);
    EXPECT_STREQ(hub.engine().activeActivityId(), "act_watch_apple_tv");
    hub.tap(Button::Music);
    EXPECT_STREQ(hub.engine().activeActivityId(), "act_play_xbox");
    take();
    hub.tap(Button::Off);
    EXPECT_FALSE(hub.engine().hasActiveActivity());
    EXPECT_TRUE(anyContains(take(), "dev_xbox media_player.turn_off"));
}

TEST_F(EngineTest, ClickOrHold) {
    hub.activate("act_watch_apple_tv");
    hub.advance(1000);
    take();
    hub.tap(Button::Info, 300);  // click: info:tap is unbound here
    EXPECT_TRUE(take().empty());
    hub.tap(Button::Info, 900);  // held past 800 ms
    EXPECT_EQ(take(), (Lines{"ble dev_apple_tv key_tap key=KEY_I"}));
}

TEST_F(EngineTest, HeldKeysAreReleasedOnActivitySwitch) {
    hub.activate("act_watch_apple_tv");
    hub.advance(1000);
    hub.press(Button::Up);
    hub.advance(100);
    ASSERT_EQ(hub.ble().heldConsumer(), 0x42);
    const size_t reports = hub.hid().reports().size();
    hub.activate("act_play_xbox");  // e.g. from the API while the thumb is still down
    EXPECT_EQ(hub.ble().heldConsumer(), 0);
    ASSERT_GT(hub.hid().reports().size(), reports);
    EXPECT_EQ(hub.hid().reports().back(), "consumer 0x0000");
    hub.release(Button::Up);  // Play Xbox has no up:up binding; nothing else happens
}

TEST_F(EngineTest, OffAlsoReleasesHeldKeys) {
    hub.activate("act_watch_apple_tv");
    hub.advance(1000);
    hub.press(Button::Number1);  // unbound, keep up held on keypad via another button
    hub.press(Button::Up);
    hub.off();
    EXPECT_EQ(hub.ble().heldConsumer(), 0);
}

TEST_F(EngineTest, LostReleaseTimesOutAndReleasesKeys) {
    hub.activate("act_watch_apple_tv");
    hub.advance(1000);
    take();
    hub.press(Button::Right);
    hub.advance(200);
    hub.advance(600, /*silent=*/true);  // release packet lost, then silence
    EXPECT_EQ(take(), (Lines{"ble dev_apple_tv key_down key=ARROW_RIGHT", "ble dev_apple_tv key_up key=ARROW_RIGHT"}));
    EXPECT_EQ(hub.ble().heldConsumer(), 0);
}

TEST_F(EngineTest, OverflowOnReleaseStillReleasesKeys) {
    ConfigSpec cfg = fixtureConfig();
    // Make every number button park the input lane so it fills up.
    for (int n = 0; n <= 9; n++) {
        cfg.activities[0].bindings.push_back(bind("number_" + std::to_string(n) + ":down", {waitMs(5000)}));
    }
    cfg.activities[0].bindings.push_back(bind("number_enter:down", {waitMs(5000)}));
    cfg.activities[0].bindings.push_back(bind("number_dot_dash:down", {waitMs(5000)}));
    ASSERT_TRUE(hub.load(cfg).ok());
    hub.activate("act_watch_apple_tv");
    hub.advance(1000);
    hub.press(Button::Up);  // key_down goes out before the lane is blocked
    ASSERT_EQ(hub.ble().heldConsumer(), 0x42);
    const Button fillers[] = {Button::Number0, Button::Number1, Button::Number2, Button::Number3,
                              Button::Number4, Button::Number5, Button::Number6, Button::Number7,
                              Button::Number8, Button::Number9, Button::NumberEnter, Button::NumberDotDash};
    // Press and release fillers repeatedly to queue >16 parked sequences.
    for (int round = 0; round < 2; round++) {
        for (Button b : fillers) {
            hub.press(b);
            hub.release(b);
        }
    }
    ASSERT_GT(hub.engine().executor().stats().rejected, 0u);
    hub.release(Button::Up);  // up:up is rejected too...
    EXPECT_GT(hub.engine().stats().overflowReleases, 0u);
    EXPECT_EQ(hub.ble().heldConsumer(), 0);  // ...but the key is released anyway
}

TEST_F(EngineTest, ConfigSwapKeepsTheActiveActivityById) {
    hub.activate("act_play_xbox");
    hub.advance(3000);
    ConfigSpec cfg = fixtureConfig();
    std::swap(cfg.activities[0], cfg.activities[1]);  // indexes change
    cfg.activities[0].bindings.push_back(bind("up:tap", {haAction("dev_xbox", "media_player.media_play_pause")}));
    cfg.revision = 2;
    ASSERT_TRUE(hub.load(cfg).ok());
    take();
    EXPECT_STREQ(hub.engine().activeActivityId(), "act_play_xbox");
    hub.tap(Button::Up);
    EXPECT_EQ(take(), (Lines{"ha_main dev_xbox media_player.media_play_pause"}));
}

namespace {
ConfigSpec withoutPlayXbox() {
    ConfigSpec cfg = fixtureConfig();
    cfg.activities.pop_back();  // act_play_xbox gone...
    // ...and music:tap, which referenced it (else the config is invalid).
    cfg.globalBindings.erase(std::remove_if(cfg.globalBindings.begin(), cfg.globalBindings.end(),
                                            [](const BindingSpec &b) { return b.trigger == "music:tap"; }),
                             cfg.globalBindings.end());
    cfg.revision = 2;
    return cfg;
}
}  // namespace

TEST_F(EngineTest, DeletingTheActiveActivitySwitchesToTheNextWithoutShutdown) {
    hub.activate("act_play_xbox");  // last in the list: "next" wraps to the first
    hub.advance(3000);
    take();
    ASSERT_TRUE(hub.load(withoutPlayXbox()).ok());
    hub.advance(3000);
    EXPECT_STREQ(hub.engine().activeActivityId(), "act_watch_apple_tv");
    const Lines lines = take();
    EXPECT_TRUE(anyContains(lines, "dev_lg_tv media_player.turn_on"));  // Apple TV startup ran
    EXPECT_FALSE(anyContains(lines, "dev_xbox"));                       // no Xbox shutdown
}

TEST_F(EngineTest, DeletingTheActiveActivityThenOffLeavesItInactive) {
    hub.activate("act_play_xbox");
    hub.advance(3000);
    take();
    // Publish without letting the engine poll, so off() is what notices the swap.
    ValidationReport report;
    auto compiled = compileConfig(withoutPlayXbox(), hub.adapters(), &report);
    ASSERT_TRUE(compiled);
    ASSERT_EQ(hub.store().publish(compiled), ErrorCode::Ok);
    EXPECT_EQ(hub.off(), ErrorCode::NoActiveActivity);
    hub.advance(3000);
    EXPECT_FALSE(hub.engine().hasActiveActivity());
    EXPECT_TRUE(take().empty());
}

TEST_F(EngineTest, DeletingTheOnlyActivityGoesInactive) {
    ConfigSpec cfg = withoutPlayXbox();
    hub.activate("act_watch_apple_tv");
    hub.advance(3000);
    ASSERT_TRUE(hub.load(cfg).ok());
    take();
    cfg.activities.clear();
    cfg.globalBindings.erase(std::remove_if(cfg.globalBindings.begin(), cfg.globalBindings.end(),
                                            [](const BindingSpec &b) {
                                                return b.trigger == "tv:tap" || b.trigger == "movie:tap";
                                            }),
                             cfg.globalBindings.end());
    cfg.revision = 3;
    ASSERT_TRUE(hub.load(cfg).ok());
    hub.advance(3000);
    EXPECT_FALSE(hub.engine().hasActiveActivity());
    EXPECT_TRUE(take().empty());
}

TEST_F(EngineTest, ConfigSwapDuringAStartupLetsItFinishFromItsCopy) {
    hub.activate("act_watch_apple_tv");
    ConfigSpec cfg = fixtureConfig();
    cfg.activities[0].startup.clear();
    ASSERT_TRUE(hub.load(cfg).ok());
    hub.advance(1000);
    EXPECT_TRUE(anyContains(take(), "source=\"HDMI 1\""));
}

TEST_F(EngineTest, StartupFailureStopsTheSequenceAndIsReported) {
    hub.homeAssistant().failNext("media_player.turn_on", ErrorCode::AdapterFailed);
    hub.activate("act_watch_apple_tv");
    hub.advance(2000);
    EXPECT_EQ(take(), (Lines{"ha_main dev_lg_tv media_player.turn_on"}));
    const std::string &last = hub.events().back().text;
    EXPECT_EQ(last, "startup act_watch_apple_tv failed adapter_failed@0");
    EXPECT_STREQ(hub.engine().activeActivityId(), "act_watch_apple_tv");  // still selected
}

TEST_F(EngineTest, BleUnavailableIsAFailureNotFakeSuccess) {
    hub.activate("act_watch_apple_tv");
    hub.advance(1000);
    hub.hid().setConnected(false);
    hub.press(Button::Select);
    ASSERT_FALSE(hub.commands().empty());
    EXPECT_EQ(hub.commands().back().text, "ble dev_apple_tv key_down key=SELECT -> failed adapter_unavailable");
}

TEST_F(EngineTest, MailboxRequestsFromAnotherThread) {
    std::thread api([&] {
        EXPECT_TRUE(hub.engine().postActivate("act_play_xbox"));
        EXPECT_FALSE(hub.engine().postActivate(std::string(40, 'x')));  // too long for an id
    });
    api.join();
    EXPECT_FALSE(hub.engine().hasActiveActivity());  // applied on the engine task only
    hub.advance(1);
    EXPECT_STREQ(hub.engine().activeActivityId(), "act_play_xbox");
    EXPECT_TRUE(hub.engine().postOff());
    hub.advance(1);
    EXPECT_FALSE(hub.engine().hasActiveActivity());
}

// Config writer thread publishes new compiled configs while the engine task
// hammers the hot path. Nothing may crash, stall or see a half-built table.
TEST_F(EngineTest, ConfigSwapDuringDispatchFromAnotherThread) {
    hub.activate("act_watch_apple_tv");
    hub.advance(1000);
    std::atomic<bool> stop{false};
    std::atomic<int> published{0};
    std::thread writer([&] {
        for (int i = 0; i < 300 && !stop; i++) {
            ConfigSpec cfg = fixtureConfig();
            cfg.revision = static_cast<uint32_t>(i + 2);
            if (i % 2) cfg.activities[0].bindings.push_back(bind("red:tap", {bleKey("key_tap", "F2")}));
            ValidationReport r;
            auto compiled = compileConfig(cfg, hub.adapters(), &r);
            ASSERT_NE(compiled, nullptr);
            while (hub.store().publish(compiled) == ErrorCode::Busy) std::this_thread::yield();
            EXPECT_EQ(compiled, nullptr);  // consumed only on success
            published++;
        }
    });
    for (int i = 0; i < 400; i++) {
        hub.press(Button::Down);
        hub.advance(2);
        hub.release(Button::Down);
        hub.advance(1);
    }
    stop = true;
    writer.join();
    hub.advance(1);
    EXPECT_STREQ(hub.engine().activeActivityId(), "act_watch_apple_tv");
    EXPECT_EQ(hub.ble().heldConsumer(), 0);
    size_t downs = 0, ups = 0;
    for (const auto &c : hub.commands()) {
        downs += c.text.find("key_down key=ARROW_DOWN") != std::string::npos;
        ups += c.text.find("key_up key=ARROW_DOWN") != std::string::npos;
    }
    EXPECT_EQ(downs, 400u);
    EXPECT_EQ(ups, 400u);
    EXPECT_GT(published.load(), 0);
}

TEST_F(EngineTest, HomeAssistantFakeRecordsTheRealServiceCall) {
    hub.activate("act_watch_apple_tv");
    hub.advance(1000);
    hub.tap(Button::Mute);
    const auto &calls = hub.homeAssistant().calls();
    ASSERT_GE(calls.size(), 5u);
    EXPECT_EQ(calls[0], "media_player.turn_on media_player.family_room_lg_tv");
    EXPECT_EQ(calls[2], "media_player.select_source media_player.family_room_lg_tv source=\"HDMI 1\"");
    EXPECT_EQ(calls.back(), "media_player.volume_mute media_player.denon is_volume_muted=true");
}
