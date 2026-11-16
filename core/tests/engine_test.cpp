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
    EXPECT_EQ(hub.ble().heldConsumer("dev_apple_tv"), 0);
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
    ASSERT_EQ(hub.ble().heldConsumer("dev_apple_tv"), 0x42);
    const size_t reports = hub.bleHosts().reports().size();
    hub.activate("act_play_xbox");  // e.g. from the API while the thumb is still down
    EXPECT_EQ(hub.ble().heldConsumer("dev_apple_tv"), 0);
    ASSERT_GT(hub.bleHosts().reports().size(), reports);
    EXPECT_EQ(hub.bleHosts().reports().back(), "consumer 0x0000");
    EXPECT_EQ(hub.engine().openHolds(), 0u);
    hub.release(Button::Up);  // Play Xbox has no up:up binding; nothing else happens
}

TEST_F(EngineTest, OffAlsoReleasesHeldKeys) {
    hub.activate("act_watch_apple_tv");
    hub.advance(1000);
    hub.press(Button::Number1);  // unbound, keep up held on keypad via another button
    hub.press(Button::Up);
    hub.off();
    EXPECT_EQ(hub.ble().heldConsumer("dev_apple_tv"), 0);
}

TEST_F(EngineTest, LostReleaseTimesOutAndReleasesKeys) {
    hub.activate("act_watch_apple_tv");
    hub.advance(1000);
    take();
    hub.press(Button::Right);
    hub.advance(200);
    hub.advance(600, /*silent=*/true);  // release packet lost, then silence
    EXPECT_EQ(take(), (Lines{"ble dev_apple_tv key_down key=ARROW_RIGHT", "ble dev_apple_tv key_up key=ARROW_RIGHT"}));
    EXPECT_EQ(hub.ble().heldConsumer("dev_apple_tv"), 0);
    EXPECT_EQ(hub.engine().openHolds(), 0u);
}

TEST_F(EngineTest, OverflowOnReleaseStillReleasesKeys) {
    ConfigSpec cfg = fixtureConfig();
    // Every number button is a slow HA call: once the in-flight table is
    // full, one parks the input lane and the rest fill it up.
    const auto slow = [] { return std::vector<ActionSpec>{haAction("dev_lights", "light.turn_on")}; };
    for (int n = 0; n <= 9; n++) {
        cfg.activities[0].bindings.push_back(sim::bind("number_" + std::to_string(n) + ":down", slow()));
    }
    cfg.activities[0].bindings.push_back(sim::bind("number_enter:down", slow()));
    cfg.activities[0].bindings.push_back(sim::bind("number_dot_dash:down", slow()));
    ASSERT_TRUE(hub.load(cfg).ok());
    hub.activate("act_watch_apple_tv");
    hub.advance(1000);
    hub.homeAssistant().setLatency(4000);
    hub.press(Button::Up);  // key_down goes out before the lane is blocked
    ASSERT_EQ(hub.ble().heldConsumer("dev_apple_tv"), 0x42);
    const Button fillers[] = {Button::Number0, Button::Number1, Button::Number2, Button::Number3,
                              Button::Number4, Button::Number5, Button::Number6, Button::Number7,
                              Button::Number8, Button::Number9, Button::NumberEnter, Button::NumberDotDash};
    // Press and release fillers repeatedly to queue >16 parked sequences.
    for (int round = 0; round < 3; round++) {
        for (Button b : fillers) {
            hub.press(b);
            hub.release(b);
        }
    }
    ASSERT_GT(hub.engine().executor().stats().rejected, 0u);
    hub.release(Button::Up);  // up:up is rejected too...
    EXPECT_GT(hub.engine().stats().forcedReleases, 0u);
    EXPECT_EQ(hub.ble().heldConsumer("dev_apple_tv"), 0);  // ...but the key is released anyway
    EXPECT_EQ(hub.engine().openHolds(), 0u);
}

namespace {

// Drops every binding of `activity` whose trigger starts with "<button>:".
void unbind(ActivitySpec &activity, const std::string &button) {
    auto &b = activity.bindings;
    b.erase(std::remove_if(b.begin(), b.end(),
                           [&](const BindingSpec &x) { return x.trigger.rfind(button + ":", 0) == 0; }),
            b.end());
}

const char *const kNoKeys = "keyboard 00 00 00 00 00 00 00 00";

}  // namespace

// Review high #2: the down comes from the global bindings, and by the up an
// activity that binds the button is active, so its up runs instead. The
// key is released anyway.
TEST_F(EngineTest, AGlobalHoldIsReleasedWhenAnActivityTakesOverTheButton) {
    ConfigSpec cfg = fixtureConfig();
    unbind(cfg.activities[0], "select");
    cfg.activities[0].bindings.push_back(bind("select:up", {haAction("dev_lights", "light.turn_on")}));
    cfg.globalBindings.push_back(bind("select:down", {bleKey("key_down", "ENTER")}));
    cfg.globalBindings.push_back(bind("select:up", {bleKey("key_up", "ENTER")}));
    ASSERT_TRUE(hub.load(cfg).ok());
    take();
    hub.press(Button::Select);  // no activity: the global down
    hub.advance(100);
    EXPECT_EQ(hub.ble().heldKeyboardKeys("dev_apple_tv"), 1u);
    EXPECT_EQ(hub.engine().openHolds(), 1u);
    hub.activate("act_watch_apple_tv");
    hub.advance(1000);
    hub.release(Button::Select);
    hub.advance(100);
    const Lines lines = take();
    EXPECT_TRUE(anyContains(lines, "ha_main dev_lights light.turn_on"));
    EXPECT_TRUE(anyContains(lines, "ble dev_apple_tv key_up key=ENTER"));
    EXPECT_EQ(hub.ble().heldKeyboardKeys("dev_apple_tv"), 0u);
    EXPECT_EQ(hub.engine().openHolds(), 0u);
    ASSERT_FALSE(hub.bleHosts().reports().empty());
    EXPECT_EQ(hub.bleHosts().reports().back(), kNoKeys);
}

// Fallback is per button: an activity's pass-through Off (:down/:up) never
// runs the global off:tap on release.
TEST_F(EngineTest, AnActivityPassThroughMasksTheGlobalTap) {
    ConfigSpec cfg = fixtureConfig();
    unbind(cfg.activities[0], "off");
    cfg.activities[0].bindings.push_back(bind("off:down", {bleKey("key_down", "MENU")}));
    cfg.activities[0].bindings.push_back(bind("off:up", {bleKey("key_up", "MENU")}));
    ASSERT_TRUE(hub.load(cfg).ok());
    hub.activate("act_watch_apple_tv");
    hub.advance(1000);
    take();
    hub.tap(Button::Off);
    hub.advance(1000);
    EXPECT_EQ(take(), (Lines{"ble dev_apple_tv key_down key=MENU", "ble dev_apple_tv key_up key=MENU"}));
    EXPECT_STREQ(hub.engine().activeActivityId(), "act_watch_apple_tv");
}

TEST(HoldLedgerTest, FixedCapacityTakeOneAtATime) {
    HoldLedger ledger;
    OpenHold hold;
    hold.button = Button::Play;
    for (size_t i = 0; i < HoldLedger::kCapacity; i++) EXPECT_TRUE(ledger.add(hold));
    EXPECT_FALSE(ledger.add(hold));
    EXPECT_EQ(ledger.size(), HoldLedger::kCapacity);
    OpenHold out;
    EXPECT_FALSE(ledger.take(Button::Up, &out));
    size_t n = 0;
    while (ledger.take(Button::Play, &out)) n++;
    EXPECT_EQ(n, HoldLedger::kCapacity);
    EXPECT_FALSE(ledger.takeAny(&out));
    EXPECT_EQ(ledger.size(), 0u);
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
    ConfigSpec cfg = fixtureConfig();
    cfg.globalBindings.erase(std::remove_if(cfg.globalBindings.begin(), cfg.globalBindings.end(),
                                            [](const BindingSpec &b) { return b.trigger == "music:tap"; }),
                             cfg.globalBindings.end());
    ASSERT_TRUE(hub.load(cfg).ok());  // nothing refers to act_play_xbox
    hub.activate("act_play_xbox");    // last in the list: "next" wraps to the first
    hub.advance(3000);
    take();
    const auto *spec = hub.configService().findActivity("act_play_xbox");
    ASSERT_NE(spec, nullptr);
    ASSERT_TRUE(hub.configService().deleteActivity("act_play_xbox", spec->revision).ok());
    hub.advance(3000);
    EXPECT_STREQ(hub.engine().activeActivityId(), "act_watch_apple_tv");
    const Lines lines = take();
    EXPECT_TRUE(anyContains(lines, "dev_lg_tv media_player.turn_on"));  // Apple TV startup ran
    EXPECT_FALSE(anyContains(lines, "dev_xbox"));                       // no Xbox shutdown
}

// Design part 6: a restore or reset never starts an activity.
TEST_F(EngineTest, ARestoreWithoutTheActiveActivityGoesInactiveSilently) {
    hub.activate("act_play_xbox");
    hub.advance(3000);
    take();
    const size_t events = hub.events().size();
    ASSERT_TRUE(hub.load(withoutPlayXbox()).ok());
    hub.advance(3000);
    EXPECT_FALSE(hub.engine().hasActiveActivity());
    EXPECT_EQ(hub.engine().activeActivityIdShared(), "");
    EXPECT_TRUE(take().empty());  // no startup of another activity, no Xbox shutdown
    EXPECT_GT(hub.events().size(), events);  // the change to "inactive" is reported
}

TEST_F(EngineTest, ARestoreKeepsTheActiveActivityIfItStillExists) {
    hub.activate("act_play_xbox");
    hub.advance(3000);
    take();
    ConfigSpec cfg = fixtureConfig();
    std::swap(cfg.activities[0], cfg.activities[1]);
    ASSERT_TRUE(hub.load(cfg).ok());
    hub.advance(3000);
    EXPECT_STREQ(hub.engine().activeActivityId(), "act_play_xbox");
    EXPECT_TRUE(take().empty());  // nothing re-run
}

TEST_F(EngineTest, AFactoryResetNeverStartsAnActivity) {
    hub.activate("act_watch_apple_tv");
    hub.advance(3000);
    take();
    ASSERT_TRUE(hub.configService().reset(withoutPlayXbox()).ok());
    hub.advance(3000);
    EXPECT_STREQ(hub.engine().activeActivityId(), "act_watch_apple_tv");  // still exists: kept
    ConfigSpec empty = withoutPlayXbox();
    empty.activities.clear();
    empty.globalBindings.erase(std::remove_if(empty.globalBindings.begin(), empty.globalBindings.end(),
                                              [](const BindingSpec &b) {
                                                  return b.trigger == "tv:tap" || b.trigger == "movie:tap";
                                              }),
                               empty.globalBindings.end());
    ASSERT_TRUE(hub.configService().reset(empty).ok());
    hub.advance(3000);
    EXPECT_FALSE(hub.engine().hasActiveActivity());
    EXPECT_TRUE(take().empty());
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

TEST_F(EngineTest, AStartupFailureIsReportedAndTheStartupGoesOn) {
    hub.homeAssistant().failNext("media_player.turn_on", ErrorCode::AdapterFailed);
    hub.activate("act_watch_apple_tv");
    hub.advance(2000);
    const Lines lines = take();
    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines.front(), "ha_main dev_lg_tv media_player.turn_on");  // failed
    EXPECT_TRUE(anyContains(lines, "source=\"Media Player\""));      // the last step still ran
    const std::string &last = hub.events().back().text;
    EXPECT_EQ(last, "startup act_watch_apple_tv completed_with_errors adapter_failed@0");
    EXPECT_STREQ(hub.engine().activeActivityId(), "act_watch_apple_tv");  // still selected
}

TEST_F(EngineTest, BleUnavailableIsAFailureNotFakeSuccess) {
    hub.activate("act_watch_apple_tv");
    hub.advance(1000);
    hub.bleHosts().setAllConnected(false);
    hub.press(Button::Select);
    ASSERT_FALSE(hub.commands().empty());
    EXPECT_EQ(hub.commands().back().text, "ble dev_apple_tv key_down key=SELECT -> failed adapter_unavailable");
}

TEST_F(EngineTest, RequestsAreAppliedOnTheEngineTaskOnly) {
    EngineRequest req;
    std::thread api([&] {
        req.activate("act_play_xbox");
        EXPECT_TRUE(hub.engine().submit(req));
    });
    api.join();
    EXPECT_FALSE(hub.engine().hasActiveActivity());  // nothing happens until the engine polls
    hub.advance(1);
    ASSERT_TRUE(req.done());
    EXPECT_STREQ(hub.engine().activeActivityId(), "act_play_xbox");
    EngineRequest tooLong;
    tooLong.activate(std::string(40, 'x'));  // can't be an id
    hub.engine().submit(tooLong);
    hub.advance(1);
    EXPECT_EQ(tooLong.transition().error, ErrorCode::UnknownActivity);
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
    EXPECT_EQ(hub.ble().heldConsumer("dev_apple_tv"), 0);
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
    // Mute is a toggle: the next press reads the muted state and unmutes.
    hub.tap(Button::Mute);
    EXPECT_EQ(calls.back(), "media_player.volume_mute media_player.denon is_volume_muted=false");
}

// ---- request/reply (EngineRequest) ----

namespace {

CompiledCommand compile(SimHub &hub, const char *device, const char *command, std::vector<ParamInput> params = {}) {
    CompiledCommand cmd;
    const config::Outcome o = hub.configService().compileCommand(device, command, params, &cmd);
    EXPECT_TRUE(o.ok()) << errorCodeName(o.code);
    return cmd;
}

}  // namespace

TEST_F(EngineTest, TransitionRequestsReplyWithTheResult) {
    EngineRequest req;
    req.activate("act_watch_apple_tv");
    ASSERT_TRUE(hub.engine().submit(req));
    EXPECT_FALSE(req.done());
    hub.advance(1);
    ASSERT_TRUE(req.done());
    EXPECT_EQ(req.transition().error, ErrorCode::Ok);
    EXPECT_EQ(req.transition().activeActivityId, "act_watch_apple_tv");
    EXPECT_TRUE(req.transition().previousActivityId.empty());
    EXPECT_EQ(req.transition().queuedActions, 5);
    EXPECT_EQ(req.transition().generation, hub.engine().generation());

    EngineRequest off;
    off.off();
    hub.engine().submit(off);
    hub.advance(1);
    ASSERT_TRUE(off.done());
    EXPECT_TRUE(off.transition().activeActivityId.empty());
    EXPECT_EQ(off.transition().previousActivityId, "act_watch_apple_tv");
    EXPECT_EQ(off.transition().queuedActions, 2);

    EngineRequest bad;
    bad.activate("act_nope");
    hub.engine().submit(bad);
    hub.advance(1);
    EXPECT_EQ(bad.transition().error, ErrorCode::UnknownActivity);

    EngineRequest none;
    none.off();
    hub.engine().submit(none);
    hub.advance(1);
    EXPECT_EQ(none.transition().error, ErrorCode::NoActiveActivity);
}

TEST_F(EngineTest, ExecuteRequestRepliesWhenTheResultIsKnown) {
    EngineRequest req;
    req.execute(compile(hub, "dev_denon", "media_player.volume_set", {param("level", 0.4)}));
    hub.engine().submit(req);
    hub.advance(1);
    ASSERT_TRUE(req.done());
    EXPECT_EQ(req.command().status, ExecStatus::Accepted);
    EXPECT_EQ(hub.homeAssistant().calls().back(), "media_player.volume_set media_player.denon volume_level=0.4");

    hub.homeAssistant().setLatency(250);
    EngineRequest slow;
    slow.execute(compile(hub, "dev_denon", "media_player.volume_up"));
    hub.engine().submit(slow);
    hub.advance(100);
    EXPECT_FALSE(slow.done());
    hub.advance(200);
    ASSERT_TRUE(slow.done());
    EXPECT_EQ(slow.command().status, ExecStatus::Accepted);
    EXPECT_EQ(slow.command().elapsedMs, 250u);

    hub.homeAssistant().setLatency(10000);
    EngineRequest lost;
    lost.execute(compile(hub, "dev_denon", "media_player.volume_up"));
    hub.engine().submit(lost);
    hub.advance(limits::kDefaultActionTimeoutMs + 1);
    ASSERT_TRUE(lost.done());
    EXPECT_EQ(lost.command().status, ExecStatus::Timeout);

    hub.homeAssistant().setLatency(0);
    hub.bleHosts().setAllConnected(false);
    EngineRequest ble;
    ble.execute(compile(hub, "dev_apple_tv", "key_tap", {param("key", "SELECT")}));
    hub.engine().submit(ble);
    hub.advance(1);
    EXPECT_EQ(ble.command().status, ExecStatus::Failed);
    EXPECT_EQ(ble.command().error, ErrorCode::AdapterUnavailable);
}

TEST_F(EngineTest, ExecuteCancelledByATransitionStillReplies) {
    hub.homeAssistant().setLatency(1000);
    EngineRequest req;
    req.execute(compile(hub, "dev_denon", "media_player.volume_up"));
    hub.engine().submit(req);
    hub.advance(1);
    EXPECT_FALSE(req.done());
    hub.activate("act_play_xbox");
    ASSERT_TRUE(req.done());
    EXPECT_EQ(req.command().status, ExecStatus::Failed);
    EXPECT_EQ(req.command().error, ErrorCode::Cancelled);
}

TEST_F(EngineTest, RequestsFromAnotherThread) {
    std::atomic<bool> stop{false};
    std::thread api([&] {
        for (int i = 0; i < 50; i++) {
            EngineRequest req;
            req.activate(i % 2 ? "act_watch_apple_tv" : "act_play_xbox");
            while (!hub.engine().submit(req)) std::this_thread::yield();
            while (!req.done()) std::this_thread::yield();
            EXPECT_EQ(req.transition().error, ErrorCode::Ok);
            EXPECT_EQ(req.transition().activeActivityId, i % 2 ? "act_watch_apple_tv" : "act_play_xbox");
        }
        stop = true;
    });
    while (!stop) hub.advance(1);
    api.join();
}

// ---- a hold closes only once its release has completed (design part 6) ----

namespace {

constexpr ParamSpec kKeyParam[] = {ParamSpec::integer("k", 0, 100)};
constexpr CommandSpec kAsyncCommands[] = {
    {"hold", kKeyParam, 1, CommandEffect::Hold, "let_go"},
    {"let_go", kKeyParam, 1},
};

// Holds synchronously; releases asynchronously (Pending, delivered by poll()).
struct AsyncHoldAdapter : IntegrationAdapter {
    const char *integrationId() const override { return "async"; }
    CommandCatalog commandsFor(const DeviceSpec &) const override { return {kAsyncCommands, 2}; }
    ExecResult execute(const ExecRequest &r) override {
        calls.push_back(std::string(r.command->id) + (r.ticket == 0 ? " direct" : ""));
        if (std::string_view(r.command->id) == "hold" || r.ticket == 0) return ExecResult::accepted();
        pending.push_back(r.ticket);
        return ExecResult::pending();
    }
    void poll(uint32_t nowMs, CompletionSink &sink) override {
        if (!deliver) return;
        for (Ticket t : pending) sink.complete(t, result, nowMs);
        pending.clear();
    }
    void releaseAll(uint32_t) override { releaseAlls++; }

    std::vector<std::string> calls;
    std::vector<Ticket> pending;
    bool deliver = false;
    ExecResult result = ExecResult::accepted();
    int releaseAlls = 0;
};

class AsyncReleaseTest : public ::testing::Test {
protected:
    AsyncReleaseTest() {
        adapters.add(adapter);
        ConfigSpec cfg;
        cfg.integrations = {integration("async", "async", "Async")};
        cfg.devices = {device("dev", "Dev", "async", "other", {"async", "ref.dev", ""})};
        cfg.globalBindings = {sim::bind("play:down", {action("async", "dev", "hold", {param("k", 5)})}),
                              sim::bind("play:up", {action("async", "dev", "let_go", {param("k", 5)})})};
        ValidationReport report;
        std::unique_ptr<CompiledConfig> compiled = compileConfig(cfg, adapters, &report);
        EXPECT_TRUE(compiled) << (report.ok() ? "" : report.issues()[0].path);
        EXPECT_EQ(store.publish(compiled), ErrorCode::Ok);
        engine.poll(0);
    }

    // Press and release play; the release is then in flight.
    void pressAndRelease() {
        engine.onRemoteEvent({Button::Play, remote::EventKind::Down, 1});
        engine.poll(2);
        engine.onRemoteEvent({Button::Play, remote::EventKind::Up, 10});
        engine.poll(11);
        ASSERT_EQ(adapter.pending.size(), 1u);
        EXPECT_EQ(engine.openHolds(), 1u);  // not forgotten while the release is pending
    }

    AsyncHoldAdapter adapter;
    AdapterSet adapters;
    Engine::Store store;
    Engine engine{store, adapters, /*actionTimeoutMs=*/500};
};

}  // namespace

TEST_F(AsyncReleaseTest, ACompletedReleaseClosesTheHold) {
    pressAndRelease();
    adapter.deliver = true;
    engine.poll(20);
    EXPECT_EQ(engine.openHolds(), 0u);
    EXPECT_EQ(adapter.releaseAlls, 0);
    EXPECT_EQ(adapter.calls, (std::vector<std::string>{"hold", "let_go"}));  // nothing forced
}

TEST_F(AsyncReleaseTest, AReleaseThatFailsLaterFallsBackToReleaseAll) {
    pressAndRelease();
    adapter.deliver = true;
    adapter.result = ExecResult::failed(ErrorCode::AdapterFailed);
    const uint32_t forced = engine.stats().forcedReleases;
    engine.poll(20);
    EXPECT_EQ(engine.openHolds(), 0u);
    EXPECT_EQ(adapter.releaseAlls, 1);
    EXPECT_EQ(engine.stats().forcedReleases, forced + 1);
}

TEST_F(AsyncReleaseTest, AReleaseThatNeverAnswersFallsBackToReleaseAll) {
    pressAndRelease();
    engine.poll(400);
    EXPECT_EQ(engine.openHolds(), 1u);
    engine.poll(600);  // past the action timeout
    EXPECT_EQ(engine.openHolds(), 0u);
    EXPECT_EQ(adapter.releaseAlls, 1);
}
