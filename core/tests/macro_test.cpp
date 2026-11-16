// Macros (docs/design/macros.md): a binding marked as a macro runs on its
// own lane, never holding up other buttons, and is recorded.
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

#include "fixtures.h"
#include "hub.h"
#include "hubbo/config/json.h"
#include "hubbo/engine/run_history.h"

using namespace hubbo;
using namespace hubbo::sim;
using remote::Button;

namespace {

ConfigSpec macroConfig() {
    ConfigSpec c = fixtureConfig();
    // Watch Apple TV: red is lights on, 2 s, lights off.
    for (ActivitySpec &a : c.activities) {
        if (a.id != "act_watch_apple_tv") continue;
        a.bindings.push_back(macro("red:down", {haAction("dev_lights", "light.turn_on"), waitMs(2000),
                                               haAction("dev_lights", "light.turn_off")}));
    }
    // Global: green is the same, from any activity.
    c.globalBindings.push_back(macro("green:hold", {haAction("dev_lights", "light.turn_on"), waitMs(500),
                                                   haAction("dev_lights", "light.turn_off")}));
    // Global: blue is a one-step macro.
    c.globalBindings.push_back(macro("blue:down", {haAction("dev_lights", "light.turn_on")}));
    // Global: guide taps are a quick volume nudge (as hubbo-api's contract case).
    c.globalBindings.push_back(macro("guide:tap", {haAction("dev_denon", "media_player.volume_up"), waitMs(300),
                                                  haAction("dev_denon", "media_player.volume_down")}));
    return c;
}

class MacroTest : public ::testing::Test {
protected:
    MacroTest() {
        const ValidationReport r = hub.load(macroConfig());
        EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.issues()[0].path);
        hub.activate("act_watch_apple_tv");
        hub.advance(3000);
        calls().clear();
    }

    std::vector<std::string> &calls() { return hub.homeAssistant().calls(); }
    size_t count(const std::string &prefix) {
        return std::count_if(calls().begin(), calls().end(),
                             [&](const std::string &c) { return c.rfind(prefix, 0) == 0; });
    }
    const ActivityRun &latest() {
        EXPECT_TRUE(hub.engine().runsShared(read.get()));
        return read->runs[0];
    }

    SimHub hub;
    std::unique_ptr<ActivityRuns> read = std::make_unique<ActivityRuns>();
};

}  // namespace

TEST_F(MacroTest, AOneStepMacroIsStillAMacro) {
    hub.tap(Button::Blue);
    hub.advance(100);
    const ActivityRun &run = latest();
    EXPECT_EQ(run.kind, SequenceOrigin::Macro);
    EXPECT_EQ(run.trigger.button, Button::Blue);
    EXPECT_EQ(run.stepCount, 1);
    EXPECT_EQ(run.outcome, RunOutcome::Completed);
    EXPECT_EQ(count("light.toggle"), 0u);
    EXPECT_EQ(count("light.turn_on"), 1u);
}

TEST_F(MacroTest, AMacroWithAWaitDoesntHoldUpAPress) {
    hub.tap(Button::Red);
    EXPECT_EQ(count("light.turn_on"), 1u);
    EXPECT_EQ(count("light.turn_off"), 0u);  // waiting
    hub.tap(Button::VolumeUp);                // one action: the Input lane
    EXPECT_EQ(count("media_player.volume_up"), 1u);
    hub.advance(2100);
    EXPECT_EQ(count("light.turn_off"), 1u);
}

TEST_F(MacroTest, AMacroRunIsRecordedWithItsButtonAndSteps) {
    hub.tap(Button::Red);
    hub.advance(2100);
    const ActivityRun &run = latest();
    EXPECT_EQ(run.kind, SequenceOrigin::Macro);
    EXPECT_STREQ(sequenceOriginName(run.kind), "macro");
    EXPECT_EQ(run.trigger.button, Button::Red);
    EXPECT_EQ(run.trigger.kind, remote::EventKind::Down);
    EXPECT_EQ(run.activityId.view(), "act_watch_apple_tv");
    EXPECT_EQ(run.outcome, RunOutcome::Completed);
    ASSERT_EQ(run.stepCount, 3);
    for (uint8_t i = 0; i < 3; i++) EXPECT_EQ(run.steps[i].result, StepResult::Ok) << int(i);
    EXPECT_GE(run.steps[1].durationMs, 2000u);  // the wait
    LastRun last;
    ASSERT_TRUE(hub.engine().lastRunShared(&last));
    EXPECT_EQ(last.kind, SequenceOrigin::Macro);
    EXPECT_EQ(last.trigger.button, Button::Red);

    // A global one has no activity; a Long press is a Hold.
    hub.press(Button::Green);
    hub.advance(1500);
    hub.release(Button::Green);
    hub.advance(1000);
    const ActivityRun &global = latest();
    EXPECT_EQ(global.kind, SequenceOrigin::Macro);
    EXPECT_TRUE(global.activityId.empty());
    EXPECT_EQ(global.trigger.button, Button::Green);
    EXPECT_EQ(global.trigger.kind, remote::EventKind::Hold);
    EXPECT_EQ(global.outcome, RunOutcome::Completed);

    // A one-action press isn't recorded.
    const uint32_t id = latest().id;
    hub.tap(Button::VolumeUp);
    hub.advance(100);
    EXPECT_EQ(latest().id, id);
}

TEST_F(MacroTest, OneRunsFourQueueAndTheNextIsRejected) {
    const uint32_t rejected = hub.engine().executor().stats().rejected;
    for (int i = 0; i < 5; i++) hub.tap(Button::Red, 10);  // one in its wait, four queued
    EXPECT_EQ(hub.engine().executor().stats().rejected, rejected);
    hub.tap(Button::Red, 10);
    EXPECT_EQ(hub.engine().executor().stats().rejected, rejected + 1);
    // Counted, not recorded: the newest run is still a queued one.
    EXPECT_EQ(latest().kind, SequenceOrigin::Macro);
    EXPECT_EQ(latest().outcome, RunOutcome::Running);
    hub.tap(Button::VolumeUp);  // never blocks the rest
    EXPECT_EQ(count("media_player.volume_up"), 1u);
}

TEST_F(MacroTest, ASwitchCancelsAMacro) {
    hub.tap(Button::Red);
    EXPECT_EQ(count("light.turn_on"), 1u);
    hub.activate("act_play_xbox");
    hub.advance(5000);
    EXPECT_EQ(count("light.turn_off"), 0u);  // never ran on into Xbox
    EXPECT_TRUE(hub.engine().runsShared(read.get()));
    const auto macro = std::find_if(read->runs, read->runs + read->count,
                                    [](const ActivityRun &r) { return r.kind == SequenceOrigin::Macro; });
    ASSERT_NE(macro, read->runs + read->count);
    EXPECT_EQ(macro->outcome, RunOutcome::Cancelled);
    EXPECT_EQ(macro->steps[2].result, StepResult::Cancelled);
}

TEST_F(MacroTest, QuickTapsAllRunInOrderAndTheOverflowIsOnlyCounted) {
    const uint32_t rejected = hub.engine().executor().stats().rejected;
    for (int i = 0; i < 6; i++) hub.tap(Button::Guide, 10);
    hub.advance(5000);
    EXPECT_TRUE(hub.engine().runsShared(read.get()));
    size_t macros = 0;
    for (uint8_t i = 0; i < read->count; i++) {
        const ActivityRun &run = read->runs[i];
        if (run.kind != SequenceOrigin::Macro) continue;
        macros++;
        EXPECT_EQ(run.outcome, RunOutcome::Completed) << "run " << run.id;
        for (uint8_t s = 0; s < run.stepCount; s++) {
            EXPECT_EQ(run.steps[s].result, StepResult::Ok) << run.id << " " << int(s);
        }
        if (i + 1 < read->count) {
            EXPECT_GT(run.id, read->runs[i + 1].id);
        }
    }
    EXPECT_EQ(macros, 5u);
    EXPECT_EQ(hub.engine().executor().stats().rejected, rejected + 1);
    EXPECT_EQ(count("media_player.volume_up"), 5u);
    EXPECT_EQ(count("media_player.volume_down"), 5u);
}

namespace {

// What POST /macros/run does: compile the body's actions, then run them.
MacroRunResult runFromEditor(SimHub &hub, const char *actionsJson, Trigger trigger, const char *activityId,
                             config::Outcome *compiled = nullptr) {
    JsonDocument doc;
    deserializeJson(doc, actionsJson);
    std::vector<CompiledCommand> steps;
    const config::Outcome outcome = hub.configService().compileMacro(doc.as<JsonVariantConst>(), &steps);
    if (compiled != nullptr) *compiled = outcome;
    if (!outcome.ok()) return {ErrorCode::ValidationFailed, 0};
    EngineRequest request;
    request.runMacro(steps.data(), steps.size(), trigger, activityId);
    EXPECT_TRUE(hub.engine().submit(request));
    while (!request.done()) hub.advance(1);
    return request.macro();
}

constexpr const char *kNudge =
    R"([{"integration_id":"ha_main","device_id":"dev_lights","command_id":"light.turn_on","params":{}},)"
    R"({"integration_id":"system","device_id":null,"command_id":"wait","params":{"duration_ms":500}},)"
    R"({"integration_id":"ha_main","device_id":"dev_lights","command_id":"light.turn_off","params":{}}])";

}  // namespace

TEST_F(MacroTest, AMacroRunsFromTheEditorUnsaved) {
    const Trigger guide{Button::Guide, remote::EventKind::Down};
    const MacroRunResult r = runFromEditor(hub, kNudge, guide, "act_play_xbox");  // not the active one
    ASSERT_EQ(r.error, ErrorCode::Ok);
    EXPECT_NE(r.runId, 0u);
    EXPECT_EQ(count("light.turn_on"), 1u);
    hub.advance(600);
    EXPECT_EQ(count("light.turn_off"), 1u);
    const ActivityRun &run = latest();
    EXPECT_EQ(run.id, r.runId);
    EXPECT_EQ(run.kind, SequenceOrigin::Macro);
    EXPECT_EQ(run.trigger, guide);
    EXPECT_EQ(run.activityId.view(), "act_play_xbox");
    EXPECT_EQ(run.outcome, RunOutcome::Completed);
    ASSERT_EQ(run.stepCount, 3);
    EXPECT_EQ(run.steps[0].deviceId.view(), "dev_lights");
    EXPECT_STREQ(run.steps[0].commandId, "light.turn_on");

    // Off: it still runs (it only sends commands).
    hub.off();
    hub.advance(3000);
    calls().clear();
    EXPECT_EQ(runFromEditor(hub, kNudge, guide, "").error, ErrorCode::Ok);
    hub.advance(600);
    EXPECT_EQ(count("light.turn_off"), 1u);
    EXPECT_TRUE(latest().activityId.empty());
}

TEST_F(MacroTest, AnEditorMacroIsValidatedAsASavedOneAndCancelledByASwitch) {
    const Trigger guide{Button::Guide, remote::EventKind::Down};
    config::Outcome compiled;
    runFromEditor(hub,
                  R"([{"integration_id":"ble","device_id":"dev_apple_tv","command_id":"key_down","params":{"key":"MENU"}},)"
                  R"({"integration_id":"ha_main","device_id":"dev_nope","command_id":"light.turn_on","params":{}}])",
                  guide, "", &compiled);
    EXPECT_EQ(compiled.code, ErrorCode::ValidationFailed);
    EXPECT_TRUE(compiled.issues.has(ErrorCode::HoldNotAllowed, "/actions/0/command_id"));
    EXPECT_FALSE(compiled.issues.ok());
    runFromEditor(hub, "[]", guide, "", &compiled);
    EXPECT_TRUE(compiled.issues.has(ErrorCode::Required, "/actions"));
    runFromEditor(hub,
                  R"([{"integration_id":"system","device_id":null,"command_id":"wait","params":{"duration_ms":500}}])",
                  guide, "", &compiled);
    EXPECT_TRUE(compiled.issues.has(ErrorCode::SequenceOnly, "/actions/0/command_id"));

    // A switch cancels it, as a pressed macro.
    ASSERT_EQ(runFromEditor(hub, kNudge, guide, "act_watch_apple_tv").error, ErrorCode::Ok);
    hub.activate("act_play_xbox");
    hub.advance(3000);
    EXPECT_EQ(count("light.turn_off"), 0u);
}

TEST(MacroCompile, AMacroIsMarkedTakesWaitsButNoHolds) {
    FakeAdapters fakes;
    ConfigSpec c = fixtureConfig();
    c.globalBindings.push_back(macro("red:down", {action("ble", "dev_apple_tv", "wait_connected", {}), waitMs(100),
                                                  haAction("dev_lights", "light.turn_on")}));
    ValidationReport ok;
    EXPECT_NE(compileConfig(c, fakes.set, &ok), nullptr) << (ok.ok() ? "" : ok.issues()[0].path);

    // A list that isn't marked a macro: macro_required, at the binding.
    ConfigSpec unmarked = fixtureConfig();
    unmarked.globalBindings.push_back(bind("red:down", {haAction("dev_lights", "light.turn_on"), waitMs(100)}));
    ValidationReport required;
    EXPECT_EQ(compileConfig(unmarked, fakes.set, &required), nullptr);
    EXPECT_TRUE(required.has(ErrorCode::MacroRequired, "/global_bindings/red:down"));

    // A single command that's only a wait is sequence_only.
    ConfigSpec plain = fixtureConfig();
    plain.globalBindings.push_back(bind("red:down", {waitMs(100)}));
    ValidationReport plainWait;
    EXPECT_EQ(compileConfig(plain, fakes.set, &plainWait), nullptr);
    EXPECT_TRUE(plainWait.has(ErrorCode::SequenceOnly, "/global_bindings/red:down/0/command_id"));
    ConfigSpec lone = fixtureConfig();
    lone.globalBindings.push_back(bind("red:down", {action("ble", "dev_apple_tv", "wait_connected", {})}));
    ValidationReport waitOnly;
    EXPECT_EQ(compileConfig(lone, fakes.set, &waitOnly), nullptr);
    EXPECT_TRUE(waitOnly.has(ErrorCode::SequenceOnly, "/global_bindings/red:down/0/command_id"));
    // So is a macro of only waits, at its first.
    ConfigSpec waits = fixtureConfig();
    waits.globalBindings.push_back(macro("red:down", {waitMs(100)}));
    waits.globalBindings.push_back(macro("blue:down", {waitMs(100), waitMs(200)}));
    ValidationReport onlyWaits;
    EXPECT_EQ(compileConfig(waits, fakes.set, &onlyWaits), nullptr);
    EXPECT_TRUE(onlyWaits.has(ErrorCode::SequenceOnly, "/global_bindings/red:down/0/command_id"));
    EXPECT_TRUE(onlyWaits.has(ErrorCode::SequenceOnly, "/global_bindings/blue:down/0/command_id"));

    // A hold, or a hold's release, in a macro (even alone): hold_not_allowed.
    ConfigSpec holds = fixtureConfig();
    holds.globalBindings.push_back(macro("red:down", {bleKey("key_down", "ARROW_UP")}));
    holds.globalBindings.push_back(macro("red:up", {bleKey("key_up", "ARROW_UP"), waitMs(100)}));
    ValidationReport refused;
    EXPECT_EQ(compileConfig(holds, fakes.set, &refused), nullptr);
    EXPECT_TRUE(refused.has(ErrorCode::HoldNotAllowed, "/global_bindings/red:down/0/command_id"));
    EXPECT_TRUE(refused.has(ErrorCode::HoldNotAllowed, "/global_bindings/red:up/0/command_id"));

    // The same with its integration disabled: disabling never changes what
    // validates.
    for (IntegrationSpec &integration : holds.integrations) {
        if (integration.id == "ble") integration.enabled = false;
    }
    ValidationReport disabled;
    EXPECT_EQ(compileConfig(holds, fakes.set, &disabled), nullptr);
    EXPECT_TRUE(disabled.has(ErrorCode::HoldNotAllowed, "/global_bindings/red:down/0/command_id"));
    EXPECT_TRUE(disabled.has(ErrorCode::HoldNotAllowed, "/global_bindings/red:up/0/command_id"));
}

TEST(MacroLane, MacrosNeverStarvePressesOfNodes) {
    FakeAdapters fakes;
    Executor executor(fakes.set);
    std::vector<CompiledCommand> steps(limits::kMaxSequenceLength);
    for (CompiledCommand &s : steps) s.action.op = ActionOp::Wait;
    SequenceInfo macro;
    macro.lane = Lane::Macro;
    macro.origin = SequenceOrigin::Macro;
    // One long macro takes the macros' whole budget; the next waits its turn.
    EXPECT_EQ(executor.enqueueSteps(macro, steps.data(), limits::kExecutorMacroNodes, 0), ErrorCode::Ok);
    EXPECT_EQ(executor.enqueueSteps(macro, steps.data(), 1, 0), ErrorCode::QueueFull);
    // Presses still have the rest of the pool.
    SequenceInfo press;
    press.lane = Lane::Input;
    press.origin = SequenceOrigin::Api;
    const size_t rest = limits::kExecutorActionPool - limits::kExecutorMacroNodes;
    EXPECT_EQ(executor.enqueueSteps(press, steps.data(), rest, 0), ErrorCode::Ok);
}

namespace {

SequenceReport finished(const SequenceInfo &info) {
    SequenceReport report;
    report.info = info;
    return report;
}

}  // namespace

TEST(MacroHistory, MacrosNeverCrowdOutStartupsOrEndARunningRun) {
    auto history = std::make_unique<RunHistory>();
    CompiledCommand step;
    step.action.op = ActionOp::Wait;
    uint32_t sequence = 1;
    const auto begin = [&](SequenceOrigin origin) {
        SequenceInfo info;
        info.id = sequence++;
        info.origin = origin;
        info.lane = origin == SequenceOrigin::Macro ? Lane::Macro : Lane::Transition;
        history->begin(info, &step, 1, ErrorCode::Ok, 0);
        return info;
    };
    const SequenceInfo done = begin(SequenceOrigin::Shutdown);
    history->onFinished(finished(done));
    const SequenceInfo running = begin(SequenceOrigin::Startup);  // a long one
    for (int i = 0; i < 20; i++) history->onFinished(finished(begin(SequenceOrigin::Macro)));
    history->publish();
    auto runs = std::make_unique<ActivityRuns>();
    history->read(runs.get());
    ASSERT_EQ(runs->count, 2 + kMaxMacroRuns);
    size_t macros = 0;
    for (uint8_t i = 0; i < runs->count; i++) {
        macros += runs->runs[i].kind == SequenceOrigin::Macro ? 1 : 0;
        if (i + 1 < runs->count) {
            EXPECT_GT(runs->runs[i].id, runs->runs[i + 1].id);  // newest first
        }
    }
    EXPECT_LE(macros, kMaxMacroRuns);
    const auto kept = [&](SequenceOrigin kind) {
        return std::any_of(runs->runs, runs->runs + runs->count, [&](const ActivityRun &r) { return r.kind == kind; });
    };
    EXPECT_TRUE(kept(SequenceOrigin::Shutdown));
    EXPECT_TRUE(kept(SequenceOrigin::Startup));
    // Its end still lands.
    history->onFinished(finished(running));
    history->publish();
    history->read(runs.get());
    for (uint8_t i = 0; i < runs->count; i++) {
        if (runs->runs[i].kind == SequenceOrigin::Startup) {
            EXPECT_EQ(runs->runs[i].outcome, RunOutcome::Completed);
        }
    }

    // Every run still running: a new one isn't kept, and none is lost.
    history->clear();
    for (size_t i = 0; i < kMaxActivityRuns; i++) begin(SequenceOrigin::Startup);
    SequenceInfo extra;
    extra.id = sequence++;
    extra.origin = SequenceOrigin::Startup;
    EXPECT_EQ(history->begin(extra, &step, 1, ErrorCode::Ok, 0), 0u);
}

TEST(MacroJson, MacrosRoundTripNextToBindings) {
    ActivitySpec a;
    a.id = "act_x";
    a.name = "X";
    a.revision = 1;
    a.bindings = {macro("guide:down", {haAction("dev_lights", "light.turn_on")}),
                  bind("info:down", {haAction("dev_lights", "light.turn_off")}),
                  macro("menu:hold", {})};  // marked but unbound: dropped
    JsonDocument doc;
    config::encodeActivity(a, doc.to<JsonObject>());
    std::string text;
    serializeJson(doc["macros"], text);
    EXPECT_EQ(text, R"(["guide:down"])");
    ActivitySpec back;
    ValidationReport report;
    ASSERT_TRUE(config::decodeActivity(doc.as<JsonVariantConst>(), config::ActivityShape::Stored, &back, &report))
        << (report.ok() ? "" : report.issues()[0].path);
    ASSERT_EQ(back.bindings.size(), 3u);
    for (const BindingSpec &b : back.bindings) EXPECT_EQ(b.macro, b.trigger == "guide:down") << b.trigger;

    // Optional (absent: none), and each entry a bound press, once.
    const auto refused = [](const char *macros, ErrorCode code, const char *path) {
        JsonDocument in;
        deserializeJson(in, std::string(R"({"name":"X","startup":[],"shutdown":[],"bindings":{"guide:down":[)"
                                        R"({"integration_id":"system","device_id":null,"command_id":"off","params":{}}]})") +
                                (macros != nullptr ? std::string(R"(,"macros":)") + macros : "") + "}");
        ActivitySpec out;
        ValidationReport r;
        EXPECT_FALSE(config::decodeActivity(in.as<JsonVariantConst>(), config::ActivityShape::Create, &out, &r));
        EXPECT_TRUE(r.has(code, path)) << (r.ok() ? "" : errorCodeName(r.issues()[0].code) + std::string(" ") +
                                                             r.issues()[0].path);
    };
    {
        JsonDocument in;
        deserializeJson(in, R"({"name":"X","startup":[],"shutdown":[],"bindings":{"guide:down":[)"
                            R"({"integration_id":"system","device_id":null,"command_id":"off","params":{}}]}})");
        ActivitySpec out;
        ValidationReport r;
        EXPECT_TRUE(config::decodeActivity(in.as<JsonVariantConst>(), config::ActivityShape::Create, &out, &r));
        ASSERT_EQ(out.bindings.size(), 1u);
        EXPECT_FALSE(out.bindings[0].macro);  // absent: no macros
    }
    refused(R"(["guide:up"])", ErrorCode::InvalidTrigger, "/macros/0");
    refused(R"(["guide:press"])", ErrorCode::InvalidTrigger, "/macros/0");
    refused(R"(["guide:down","guide:down"])", ErrorCode::DuplicateTrigger, "/macros/1");
    refused(R"(["info:hold"])", ErrorCode::UnboundMacro, "/macros/0");
}
