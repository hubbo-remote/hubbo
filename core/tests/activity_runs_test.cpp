// Activity runs (docs/design/activity-runs.md): startups and shutdowns go on
// past a failed step, and the last few are kept step by step.
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

#include "fake_ha.h"
#include "fixtures.h"
#include "hub.h"
#include "hubbo/engine/run_history.h"

using namespace hubbo;
using namespace hubbo::sim;
using remote::Button;

namespace {

class ActivityRunsTest : public ::testing::Test {
protected:
    ActivityRunsTest() { EXPECT_TRUE(hub.load(fixtureConfig()).ok()); }

    // The published runs, newest first.
    const ActivityRuns &runs() {
        EXPECT_TRUE(hub.engine().runsShared(read.get()));
        return *read;
    }
    const RunStep &step(size_t run, size_t index) { return runs().runs[run].steps[index]; }

    SimHub hub;
    std::unique_ptr<ActivityRuns> read = std::make_unique<ActivityRuns>();
};

}  // namespace

// Watch Apple TV's startup: LG on, Denon on, wait 1 s, LG input, Denon input.
TEST_F(ActivityRunsTest, AFailingMiddleStepStillRunsTheRest) {
    hub.homeAssistant().failNext("media_player.select_source", ErrorCode::AdapterFailed);  // the LG's, step 3
    hub.activate("act_watch_apple_tv");
    hub.advance(100);
    const ActivityRun &running = runs().runs[0];
    EXPECT_EQ(running.outcome, RunOutcome::Running);
    EXPECT_EQ(running.steps[3].result, StepResult::Pending);
    EXPECT_FALSE(running.steps[3].ran);

    hub.advance(1000);
    ASSERT_EQ(runs().count, 1);
    const ActivityRun &run = runs().runs[0];
    EXPECT_EQ(run.activityId.view(), "act_watch_apple_tv");
    EXPECT_EQ(run.kind, SequenceOrigin::Startup);
    EXPECT_EQ(run.outcome, RunOutcome::CompletedWithErrors);
    EXPECT_EQ(run.durationMs, 1000u);
    ASSERT_EQ(run.stepCount, 5);
    EXPECT_EQ(run.failedSteps(), 1);
    EXPECT_EQ(run.steps[0].deviceId.view(), "dev_lg_tv");
    EXPECT_STREQ(run.steps[0].commandId, "media_player.turn_on");
    EXPECT_EQ(run.steps[0].result, StepResult::Ok);
    EXPECT_EQ(run.steps[2].deviceId.view(), "");  // system.wait
    EXPECT_STREQ(run.steps[2].commandId, "wait");
    EXPECT_EQ(run.steps[3].result, StepResult::Failed);
    EXPECT_EQ(run.steps[3].error, ErrorCode::AdapterFailed);
    EXPECT_TRUE(run.steps[3].ran);
    EXPECT_EQ(run.steps[4].result, StepResult::Ok);  // the Denon's input still changed
    EXPECT_EQ(run.steps[4].error, ErrorCode::Ok);

    // In brief (/status), the same newest run.
    LastRun last;
    ASSERT_TRUE(hub.engine().lastRunShared(&last));
    EXPECT_EQ(last.activityId.view(), "act_watch_apple_tv");
    EXPECT_EQ(last.kind, SequenceOrigin::Startup);
    EXPECT_EQ(last.outcome, RunOutcome::CompletedWithErrors);
    EXPECT_EQ(last.failedSteps, 1);
}

TEST_F(ActivityRunsTest, ATimedOutStepThenTheNext) {
    hub.homeAssistant().failNext("media_player.turn_on", ErrorCode::AdapterTimeout);
    hub.activate("act_watch_apple_tv");
    hub.advance(1100);
    EXPECT_EQ(step(0, 0).result, StepResult::TimedOut);
    EXPECT_EQ(step(0, 0).error, ErrorCode::AdapterTimeout);
    EXPECT_EQ(step(0, 1).result, StepResult::Ok);
    EXPECT_EQ(runs().runs[0].outcome, RunOutcome::CompletedWithErrors);

    // No answer at all: the step times out after 5 s, then the next starts.
    hub.homeAssistant().setLatency(60000);
    hub.activate("act_play_xbox");
    hub.advance(4000);
    EXPECT_EQ(step(0, 0).result, StepResult::Pending);
    EXPECT_TRUE(step(0, 0).ran);
    EXPECT_FALSE(step(0, 1).ran);
    hub.advance(1000);
    EXPECT_EQ(step(0, 0).result, StepResult::TimedOut);
    EXPECT_EQ(step(0, 0).durationMs, 5000u);
    EXPECT_TRUE(step(0, 1).ran);
    EXPECT_EQ(step(0, 1).result, StepResult::Pending);
}

TEST_F(ActivityRunsTest, ASwitchMidRunCancelsTheRest) {
    hub.activate("act_watch_apple_tv");
    hub.advance(500);  // in its 1 s wait
    hub.activate("act_play_xbox");
    hub.advance(100);
    ASSERT_EQ(runs().count, 2);
    const ActivityRun &watch = runs().runs[1];
    EXPECT_EQ(watch.outcome, RunOutcome::Cancelled);
    EXPECT_EQ(watch.steps[1].result, StepResult::Ok);
    for (size_t i = 3; i < 5; i++) {
        EXPECT_EQ(watch.steps[i].result, StepResult::Cancelled);
        EXPECT_EQ(watch.steps[i].error, ErrorCode::Cancelled);
        EXPECT_FALSE(watch.steps[i].ran);
    }
    EXPECT_EQ(watch.failedSteps(), 0);
    EXPECT_EQ(runs().runs[0].activityId.view(), "act_play_xbox");
    EXPECT_EQ(runs().runs[0].outcome, RunOutcome::Running);
}

TEST_F(ActivityRunsTest, OffIsAShutdownRun) {
    hub.activate("act_watch_apple_tv");
    hub.advance(2000);
    hub.off();
    hub.advance(100);
    const ActivityRun &off = runs().runs[0];
    EXPECT_EQ(off.kind, SequenceOrigin::Shutdown);
    EXPECT_EQ(off.activityId.view(), "act_watch_apple_tv");
    EXPECT_EQ(off.outcome, RunOutcome::Completed);
    LastRun last;  // the newest of the two
    ASSERT_TRUE(hub.engine().lastRunShared(&last));
    EXPECT_EQ(last.kind, SequenceOrigin::Shutdown);
}

TEST_F(ActivityRunsTest, TheHistoryKeepsTheLastEight) {
    for (int i = 0; i < 6; i++) {
        hub.activate(i % 2 == 0 ? "act_watch_apple_tv" : "act_play_xbox");
        hub.advance(3000);
        hub.off();
        hub.advance(3000);
    }
    const ActivityRuns &all = runs();
    ASSERT_EQ(all.count, kMaxActivityRuns);
    EXPECT_EQ(all.runs[0].kind, SequenceOrigin::Shutdown);  // newest first
    EXPECT_EQ(all.runs[0].activityId.view(), "act_play_xbox");
    EXPECT_EQ(all.runs[1].kind, SequenceOrigin::Startup);
    EXPECT_EQ(all.runs[1].activityId.view(), "act_play_xbox");
    EXPECT_EQ(all.runs[7].kind, SequenceOrigin::Startup);  // the 5th of 12; the first 4 are gone
    EXPECT_EQ(all.runs[7].activityId.view(), "act_watch_apple_tv");
    for (size_t i = 0; i < kMaxActivityRuns; i++) EXPECT_EQ(all.runs[i].outcome, RunOutcome::Completed);
}

TEST_F(ActivityRunsTest, ButtonPressesAreNotRecorded) {
    hub.activate("act_watch_apple_tv");
    hub.advance(2000);
    hub.tap(Button::VolumeUp);
    hub.tap(Button::Light1);
    hub.advance(1000);
    EXPECT_EQ(runs().count, 1);
    EXPECT_EQ(runs().runs[0].outcome, RunOutcome::Completed);
}

TEST(ActivityRuns, NothingWithoutAHistory) {
    FakeAdapters fakes;
    Engine::Store store;
    Engine engine(store, fakes.set);
    ActivityRuns out;
    EXPECT_FALSE(engine.runsShared(&out));
    LastRun last;
    EXPECT_FALSE(engine.lastRunShared(&last));
    // A history with no runs yet.
    RunHistory history;
    EXPECT_FALSE(history.readLatest(&last));
}

TEST(ActivityRuns, ASpanThatDoesntFitRecordsNoSteps) {
    // As enqueue() would refuse it (TooManyActions): the run is listed,
    // rejected, with nothing read past the config's actions.
    CompiledConfig config;
    SequenceInfo info;
    info.origin = SequenceOrigin::Startup;
    info.activityId.assign("act_x");
    auto history = std::make_unique<RunHistory>();
    history->begin(info, config, ActionSpan{0, 3}, ErrorCode::TooManyActions, 0);
    history->publish();
    auto out = std::make_unique<ActivityRuns>();
    history->read(out.get());
    ASSERT_EQ(out->count, 1);
    EXPECT_EQ(out->runs[0].stepCount, 0);
    EXPECT_EQ(out->runs[0].outcome, RunOutcome::Rejected);
}

TEST(StepOutcome, MapsEachAdapterResult) {
    auto is = [](ExecResult r, StepResult result, ErrorCode error) {
        const StepOutcome out = stepOutcome(r);
        EXPECT_EQ(out.result, result) << execStatusName(r.status);
        EXPECT_EQ(out.error, error) << execStatusName(r.status);
    };
    is(ExecResult::pending(), StepResult::Pending, ErrorCode::Ok);
    is(ExecResult::accepted(), StepResult::Ok, ErrorCode::Ok);
    // A wait_connected that gave up while its sequence goes on: ok, with why.
    is({ExecStatus::Accepted, ErrorCode::AdapterUnavailable}, StepResult::Ok, ErrorCode::AdapterUnavailable);
    is(ExecResult::failed(ErrorCode::AdapterUnavailable), StepResult::Failed, ErrorCode::AdapterUnavailable);
    is(ExecResult::failed(ErrorCode::Ok), StepResult::Failed, ErrorCode::AdapterFailed);  // failureCode()
    is(ExecResult::timeout(), StepResult::TimedOut, ErrorCode::AdapterTimeout);
    is(ExecResult::skipped(), StepResult::Skipped, ErrorCode::AlreadySet);
    is(ExecResult::noReply(), StepResult::NoReply, ErrorCode::Ok);
}

// ---- state checks and the 1 s reply limit, through the real HA client ------

namespace {

// The fixture world on the real HA client and adapter, over the fake HA
// server (whose media players start off).
class StateChecksTest : public ::testing::Test {
protected:
    StateChecksTest() {
        ConfigSpec config = fixtureConfig();
        addFakeHaSnapshots(&config, hub.haServer());
        EXPECT_TRUE(hub.load(config).ok());
        hub.haClient().configure("ws://ha.local:8123/api/websocket", hub.haServer().token);
        hub.advance(500);
        EXPECT_EQ(hub.haClient().state(), integrations::HaState::Connected);
    }

    const ActivityRun &latest() {
        EXPECT_TRUE(hub.engine().runsShared(read.get()));
        return read->runs[0];
    }
    size_t sent(const std::string &prefix) {
        size_t n = 0;
        for (const std::string &c : hub.haServer().calls()) n += c.rfind(prefix, 0) == 0 ? 1 : 0;
        return n;
    }

    SimHub hub{nullptr, nullptr, /*realHomeAssistant=*/true};
    std::unique_ptr<ActivityRuns> read = std::make_unique<ActivityRuns>();
};

constexpr const char *kDenon = "media_player.denon";

}  // namespace

// Watch Apple TV: LG on, Denon on, wait 1 s, LG HDMI 1, Denon Media Player.
// Play Xbox: Xbox on, LG on, Denon on, wait 2 s, LG HDMI 2, Denon Game.
TEST_F(StateChecksTest, FromOffEveryStepRunsThenASwitchSkipsWhatsOn) {
    hub.activate("act_watch_apple_tv");
    hub.advance(3000);
    const ActivityRun &watch = latest();
    EXPECT_EQ(watch.outcome, RunOutcome::Completed);
    for (size_t i : {0, 1, 3, 4}) EXPECT_EQ(watch.steps[i].result, StepResult::Ok) << i;
    EXPECT_EQ(sent("media_player.turn_on"), 2u);

    // The TV and receiver are on: their power is skipped, the inputs change.
    hub.activate("act_play_xbox");
    hub.advance(4000);
    const ActivityRun &xbox = latest();
    EXPECT_EQ(xbox.activityId.view(), "act_play_xbox");
    EXPECT_EQ(xbox.outcome, RunOutcome::Completed);
    EXPECT_EQ(xbox.failedSteps(), 0);
    EXPECT_EQ(xbox.steps[0].result, StepResult::Ok);  // the Xbox was off
    for (size_t i : {1, 2}) {
        EXPECT_EQ(xbox.steps[i].result, StepResult::Skipped) << i;
        EXPECT_EQ(xbox.steps[i].error, ErrorCode::AlreadySet) << i;
        EXPECT_TRUE(xbox.steps[i].ran) << i;
    }
    EXPECT_EQ(xbox.steps[4].result, StepResult::Ok);  // HDMI 1 -> HDMI 2
    EXPECT_EQ(xbox.steps[5].result, StepResult::Ok);  // Media Player -> Game
    EXPECT_EQ(sent("media_player.turn_on"), 3u);       // only the Xbox's
    EXPECT_EQ(sent("media_player.select_source"), 4u);

    // Again: the inputs are already right too.
    hub.activate("act_play_xbox");
    hub.advance(4000);
    for (size_t i : {0, 1, 2, 4, 5}) EXPECT_EQ(latest().steps[i].result, StepResult::Skipped) << i;
    EXPECT_EQ(sent("media_player.select_source"), 4u);
}

TEST_F(StateChecksTest, ASlowReplyIsNoReplyAfterOneSecondAndTheRunCompletes) {
    // The Denon is on, on the TV input, and answers after 5 s.
    ASSERT_TRUE(hub.haServer().setState(kDenon, "on"));
    ASSERT_TRUE(hub.haServer().setAttribute(kDenon, "source", "\"TV Audio\""));
    hub.haServer().slow[kDenon] = 5000;
    hub.activate("act_play_xbox");
    hub.advance(2600);  // the power steps and the 2 s wait, LG HDMI 2
    hub.advance(1100);  // the Denon's input: no reply within 1 s
    const ActivityRun &run = latest();
    EXPECT_EQ(run.steps[2].result, StepResult::Skipped);  // the Denon's power
    EXPECT_EQ(run.steps[5].result, StepResult::NoReply);
    EXPECT_EQ(run.steps[5].error, ErrorCode::Ok);
    EXPECT_GE(run.steps[5].durationMs, 1000u);
    EXPECT_LE(run.steps[5].durationMs, 1400u);  // the read, then 1 s
    EXPECT_EQ(run.outcome, RunOutcome::Completed);
    EXPECT_EQ(run.failedSteps(), 0);
    LastRun last;
    ASSERT_TRUE(hub.engine().lastRunShared(&last));
    EXPECT_EQ(last.outcome, RunOutcome::Completed);
    EXPECT_EQ(last.failedSteps, 0);
    hub.advance(5000);  // the late answer changes nothing
    EXPECT_EQ(latest().steps[5].result, StepResult::NoReply);
}

TEST_F(StateChecksTest, AFastErrorStillFails) {
    hub.haServer().failing.insert("media_player.select_source");
    hub.activate("act_watch_apple_tv");
    hub.advance(3000);
    const ActivityRun &run = latest();
    EXPECT_EQ(run.steps[3].result, StepResult::Failed);
    EXPECT_EQ(run.steps[4].result, StepResult::Failed);
    EXPECT_EQ(run.outcome, RunOutcome::CompletedWithErrors);
    EXPECT_EQ(run.failedSteps(), 2);
}

TEST_F(StateChecksTest, OffRightAfterAnActivateLeavesEverythingOff) {
    // Off before the startup's first step is sent: the switch cancels it
    // in the HA adapter too, so it's never sent and the shutdown's checks
    // see everything still off. (The adapter's tests cover a cancel during
    // the step's read.)
    hub.activate("act_watch_apple_tv");
    hub.off();
    hub.advance(3000);
    EXPECT_EQ(sent("media_player.turn_on"), 0u);
    EXPECT_EQ(sent("media_player.turn_off"), 0u);
    const ActivityRun &off = latest();
    EXPECT_EQ(off.kind, SequenceOrigin::Shutdown);
    for (size_t i : {0, 1}) EXPECT_EQ(off.steps[i].result, StepResult::Skipped) << i;
    EXPECT_EQ(read->runs[1].outcome, RunOutcome::Cancelled);
}

TEST_F(StateChecksTest, RunsAreNumberedAndAWaitHasItsLength) {
    hub.activate("act_watch_apple_tv");
    hub.advance(500);  // in the 1 s wait
    const ActivityRun &running = latest();
    EXPECT_TRUE(running.steps[2].ran);
    EXPECT_FALSE(running.steps[2].hasDuration());  // still waiting
    EXPECT_FALSE(running.steps[3].hasDuration());  // not run yet
    const uint32_t first = running.id;
    EXPECT_NE(first, 0u);
    hub.advance(2500);
    const ActivityRun &done = latest();
    EXPECT_EQ(done.steps[2].result, StepResult::Ok);
    EXPECT_TRUE(done.steps[2].hasDuration());
    EXPECT_GE(done.steps[2].durationMs, 1000u);
    EXPECT_LE(done.steps[2].durationMs, 1010u);
    // The same activity again: a new run, a new id.
    hub.activate("act_watch_apple_tv");
    hub.advance(3000);
    EXPECT_EQ(latest().id, first + 1);
    LastRun last;
    ASSERT_TRUE(hub.engine().lastRunShared(&last));
    EXPECT_EQ(last.id, first + 1);
}

TEST_F(StateChecksTest, ButtonPressesNeverCheck) {
    hub.activate("act_watch_apple_tv");
    hub.advance(3000);
    const auto reads = [&] {
        const auto &received = hub.haServer().received();
        return std::count_if(received.begin(), received.end(), [](const std::string &m) {
            return m.find(R"("type":"subscribe_entities")") != std::string::npos;
        });
    };
    const size_t calls = hub.haServer().calls().size();
    const auto before = reads();
    hub.tap(Button::VolumeUp);
    hub.advance(200);
    EXPECT_EQ(hub.haServer().calls().size(), calls + 1);  // sent, with no read first
    EXPECT_EQ(reads(), before);
}
