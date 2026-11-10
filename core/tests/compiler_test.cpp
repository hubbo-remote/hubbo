// Validation and compilation of activities into lookup tables.
#include <gtest/gtest.h>

#include "fakes.h"
#include "fixtures.h"
#include "hubbo/engine/compiler.h"
#include "hubbo/integrations/ble_hid.h"

using namespace hubbo;
using namespace hubbo::sim;
using remote::Button;
using remote::EventKind;

namespace {

class CompilerTest : public ::testing::Test {
protected:
    std::unique_ptr<CompiledConfig> compile(const ConfigSpec &c) { return compileConfig(c, adapters, &report); }

    // Compiles the fixture with one action replaced and returns the report.
    ValidationReport withStartupAction(ActionSpec a) {
        ConfigSpec c = fixtureConfig();
        c.activities[0].startup = {std::move(a)};
        ValidationReport r;
        EXPECT_EQ(compileConfig(c, adapters, &r), nullptr);
        return r;
    }

    FakeAdapters fakes;
    AdapterSet &adapters = fakes.set;
    ValidationReport report;
};

const char *kStartup0 = "/activities/0/startup/0";

}  // namespace

TEST_F(CompilerTest, FixtureCompiles) {
    auto c = compile(fixtureConfig());
    ASSERT_NE(c, nullptr);
    EXPECT_TRUE(report.ok());
    ASSERT_EQ(c->activities.size(), 2u);
    EXPECT_EQ(c->findActivity("act_watch_apple_tv"), 0);
    EXPECT_EQ(c->findActivity("act_play_xbox"), 1);
    EXPECT_EQ(c->findActivity("nope"), -1);
    EXPECT_EQ(c->activities[0].name, "Watch Apple TV");
    EXPECT_EQ(c->activities[0].startup.count, 5);
    EXPECT_EQ(c->activities[1].shutdown.count, 3);
}

TEST_F(CompilerTest, BindingsBecomeFixedIndexSpans) {
    auto c = compile(fixtureConfig());
    ASSERT_NE(c, nullptr);
    const ActionSpan up = c->lookup(0, Button::Up, EventKind::Down);
    ASSERT_EQ(up.count, 1);
    const CompiledAction &a = c->span(up)[0];
    EXPECT_EQ(a.op, ActionOp::Adapter);
    EXPECT_EQ(a.adapterIndex, 0);  // ble
    EXPECT_STREQ(a.command->id, "key_down");
    EXPECT_EQ(a.commandIndex, static_cast<uint16_t>(integrations::BleHidCommand::KeyDown));
    EXPECT_STREQ(integrations::bleHidKey(a.params[0].asOption()).name, "ARROW_UP");
    EXPECT_EQ(c->devices[a.deviceIndex].id, "dev_apple_tv");

    // System wait compiles to a Wait op with its duration.
    const CompiledAction &wait = c->span(c->activities[0].startup)[2];
    EXPECT_EQ(wait.op, ActionOp::Wait);
    EXPECT_EQ(wait.params[0].integer, 1000);
}

TEST_F(CompilerTest, UnboundTriggersAreEmptyAndGlobalsApplyUnderActivities) {
    auto c = compile(fixtureConfig());
    ASSERT_NE(c, nullptr);
    bool global = false;
    EXPECT_EQ(c->lookup(0, Button::Number5, EventKind::Down, &global).count, 0);  // unbound anywhere
    EXPECT_EQ(c->lookup(-1, Button::Up, EventKind::Down).count, 0);  // no activity: only globals
    const ActionSpan off = c->lookup(0, Button::Off, EventKind::Tap, &global);
    ASSERT_EQ(off.count, 1);
    EXPECT_TRUE(global);
    EXPECT_EQ(c->span(off)[0].op, ActionOp::Off);
    c->lookup(0, Button::Up, EventKind::Down, &global);
    EXPECT_FALSE(global);
    // Out-of-range indexes are harmless.
    EXPECT_EQ(c->lookup(99, Button::Up, EventKind::Down).count, 0);
    EXPECT_EQ(c->lookup(0, Button::Count, EventKind::Down).count, 0);
}

TEST_F(CompilerTest, ActivityBindingOverridesGlobal) {
    ConfigSpec cfg = fixtureConfig();
    cfg.activities[0].bindings.push_back(bind("off:tap", {bleKey("key_tap", "MENU")}));
    auto c = compile(cfg);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->span(c->lookup(0, Button::Off, EventKind::Tap))[0].op, ActionOp::Adapter);
    EXPECT_EQ(c->span(c->lookup(1, Button::Off, EventKind::Tap))[0].op, ActionOp::Off);
}

TEST_F(CompilerTest, EmptyArraysAreValid) {
    ConfigSpec cfg = fixtureConfig();
    cfg.activities[0].startup.clear();
    cfg.activities[0].shutdown.clear();
    cfg.activities[0].bindings.push_back(bind("number_1:down", {}));  // empty == unbound
    auto c = compile(cfg);
    ASSERT_NE(c, nullptr) << (report.ok() ? "" : report.issues()[0].path);
    EXPECT_EQ(c->activities[0].startup.count, 0);
    EXPECT_EQ(c->lookup(0, Button::Number1, EventKind::Down).count, 0);

    ConfigSpec empty;
    auto e = compile(empty);
    ASSERT_NE(e, nullptr);
    EXPECT_TRUE(e->activities.empty());
}

TEST_F(CompilerTest, MalformedIds) {
    EXPECT_TRUE(withStartupAction(action("HA", "dev_lg_tv", "media_player.turn_on"))
                    .has(ErrorCode::InvalidId, std::string(kStartup0) + "/integration_id"));
    EXPECT_TRUE(withStartupAction(action("ha_main", "dev lg", "media_player.turn_on"))
                    .has(ErrorCode::InvalidId, std::string(kStartup0) + "/device_id"));
    EXPECT_TRUE(withStartupAction(action("ha_main", "dev_lg_tv", "media_player.turn.on"))
                    .has(ErrorCode::InvalidCommandId, std::string(kStartup0) + "/command_id"));

    ConfigSpec cfg = fixtureConfig();
    cfg.activities[1].id = "Play Xbox";
    cfg.activities[0].name = "";
    cfg.devices[0].id = "9dev";
    compile(cfg);
    EXPECT_TRUE(report.has(ErrorCode::InvalidId, "/activities/1/id"));
    EXPECT_TRUE(report.has(ErrorCode::InvalidName, "/activities/0/name"));
    EXPECT_TRUE(report.has(ErrorCode::InvalidId, "/devices/0/id"));
}

TEST_F(CompilerTest, UnregisteredTargetsAreRejected) {
    EXPECT_TRUE(withStartupAction(action("hass", "dev_lg_tv", "media_player.turn_on"))
                    .has(ErrorCode::UnknownIntegration, std::string(kStartup0) + "/integration_id"));
    EXPECT_TRUE(withStartupAction(action("ha_main", "dev_ghost", "media_player.turn_on"))
                    .has(ErrorCode::UnknownDevice, std::string(kStartup0) + "/device_id"));
    EXPECT_TRUE(withStartupAction(action("ha_main", "", "media_player.turn_on"))
                    .has(ErrorCode::DeviceRequired, std::string(kStartup0) + "/device_id"));
    // The Apple TV belongs to "ble", not "ha_main".
    EXPECT_TRUE(withStartupAction(action("ha_main", "dev_apple_tv", "media_player.turn_on"))
                    .has(ErrorCode::IntegrationMismatch, std::string(kStartup0) + "/device_id"));
    // A light doesn't offer media_player commands; arbitrary services aren't a thing.
    EXPECT_TRUE(withStartupAction(action("ha_main", "dev_lights", "media_player.turn_on"))
                    .has(ErrorCode::UnknownCommand, std::string(kStartup0) + "/command_id"));
    EXPECT_TRUE(withStartupAction(action("ha_main", "dev_lg_tv", "homeassistant.restart"))
                    .has(ErrorCode::UnknownCommand, std::string(kStartup0) + "/command_id"));
    // System actions take no device.
    ActionSpec wait = waitMs(10);
    wait.deviceId = "dev_lg_tv";
    EXPECT_TRUE(withStartupAction(wait).has(ErrorCode::DeviceNotAllowed, std::string(kStartup0) + "/device_id"));
    EXPECT_TRUE(withStartupAction(action("system", "", "sleep"))
                    .has(ErrorCode::UnknownCommand, std::string(kStartup0) + "/command_id"));
}

TEST_F(CompilerTest, InvalidParams) {
    const std::string p = std::string(kStartup0) + "/params";
    EXPECT_TRUE(withStartupAction(bleKey("key_down", "ARROW_SIDEWAYS")).has(ErrorCode::InvalidEnumValue, p + "/key"));
    EXPECT_TRUE(withStartupAction(action("ble", "dev_apple_tv", "key_down")).has(ErrorCode::MissingParam, p + "/key"));
    EXPECT_TRUE(withStartupAction(waitMs(60001)).has(ErrorCode::ParamOutOfRange, p + "/duration_ms"));
    EXPECT_TRUE(withStartupAction(haAction("dev_denon", "media_player.volume_set", {param("level", 1.5)}))
                    .has(ErrorCode::ParamOutOfRange, p + "/level"));
    EXPECT_TRUE(withStartupAction(haAction("dev_denon", "media_player.volume_set", {param("level", "loud")}))
                    .has(ErrorCode::InvalidParamType, p + "/level"));
    // Nested target overrides are just unknown params.
    EXPECT_TRUE(withStartupAction(haAction("dev_denon", "media_player.turn_on", {param("entity_id", "all")}))
                    .has(ErrorCode::UnknownParam, p + "/entity_id"));
    EXPECT_TRUE(withStartupAction(action("system", "", "activate", {param("activity_id", "nope")}))
                    .has(ErrorCode::UnknownActivity, p + "/activity_id"));
}

TEST_F(CompilerTest, TriggerErrors) {
    ConfigSpec cfg = fixtureConfig();
    cfg.activities[0].bindings.push_back(bind("up:press", {bleKey("key_tap", "MENU")}));
    cfg.activities[0].bindings.push_back(bind("up:down", {bleKey("key_tap", "MENU")}));  // dup of fixture's
    cfg.globalBindings.push_back(bind("a/b~c", {offAction()}));
    compile(cfg);
    EXPECT_TRUE(report.has(ErrorCode::InvalidTrigger, "/activities/0/bindings/up:press"));
    EXPECT_TRUE(report.has(ErrorCode::DuplicateTrigger, "/activities/0/bindings/up:down"));
    EXPECT_TRUE(report.has(ErrorCode::InvalidTrigger, "/global_bindings/a~1b~0c"));
}

TEST_F(CompilerTest, TransitionsOnlyLastInABinding) {
    ConfigSpec cfg = fixtureConfig();
    cfg.activities[0].startup.push_back(activateAction("act_play_xbox"));
    cfg.activities[1].shutdown.insert(cfg.activities[1].shutdown.begin(), offAction());
    cfg.activities[0].bindings.push_back(bind("red:tap", {offAction(), bleKey("key_tap", "MENU")}));
    cfg.activities[0].bindings.push_back(bind("green:tap", {bleKey("key_tap", "MENU"), activateAction("act_play_xbox")}));
    compile(cfg);
    EXPECT_TRUE(report.has(ErrorCode::MisplacedTransition, "/activities/0/startup/5/command_id"));
    EXPECT_TRUE(report.has(ErrorCode::MisplacedTransition, "/activities/1/shutdown/0/command_id"));
    EXPECT_TRUE(report.has(ErrorCode::MisplacedTransition, "/activities/0/bindings/red:tap/0/command_id"));
    EXPECT_EQ(report.issues().size(), 3u);  // green:tap is fine
}

TEST_F(CompilerTest, DuplicateIds) {
    ConfigSpec cfg = fixtureConfig();
    cfg.activities[1].id = "act_watch_apple_tv";
    cfg.devices.push_back(haDevice("dev_denon", "Other", "receiver", "media_player.other"));
    compile(cfg);
    EXPECT_TRUE(report.has(ErrorCode::DuplicateId, "/activities/1/id"));
    EXPECT_TRUE(report.has(ErrorCode::DuplicateId, "/devices/5/id"));
}

TEST_F(CompilerTest, DeviceRegistryChecks) {
    ConfigSpec cfg = fixtureConfig();
    cfg.devices.push_back(device("dev_sys", "Sys", "system", "other", {"system", "x", "", ""}));
    cfg.devices.push_back(haDevice("dev_noref", "No ref", "tv", ""));
    cfg.devices.push_back(device("dev_wrong", "Wrong", "ha_main", "tv", {"ble_hid", "", "", "x"}));
    cfg.integrations.push_back(integration("ha_main", "home_assistant", "Duplicate"));
    cfg.integrations.push_back(integration("ghost", "home_assistant", "Ghost"));
    compile(cfg);
    EXPECT_TRUE(report.has(ErrorCode::UnknownIntegration, "/devices/5/integration_id"));
    EXPECT_TRUE(report.has(ErrorCode::Required, "/devices/6/provider"));
    EXPECT_TRUE(report.has(ErrorCode::IntegrationMismatch, "/devices/7/provider/type"));
    EXPECT_TRUE(report.has(ErrorCode::DuplicateId, "/integrations/3/id"));
    // A listed integration without an adapter is fine until a device uses it.
    EXPECT_FALSE(report.has(ErrorCode::UnknownIntegration, "/integrations/4/id"));
}

TEST_F(CompilerTest, Bounds) {
    ConfigSpec cfg = fixtureConfig();
    cfg.activities[0].startup.assign(limits::kMaxSequenceLength + 1, waitMs(1));
    compile(cfg);
    EXPECT_TRUE(report.has(ErrorCode::TooManyActions, "/activities/0/startup"));

    ConfigSpec many = fixtureConfig();
    for (size_t i = 0; i < limits::kMaxActivities; i++) {
        ActivitySpec a = many.activities[0];
        a.id = "extra_" + std::to_string(i);
        many.activities.push_back(a);
    }
    report = {};
    EXPECT_EQ(compile(many), nullptr);
    EXPECT_TRUE(report.has(ErrorCode::TooManyActivities, "/activities"));

    ConfigSpec devices = fixtureConfig();
    for (size_t i = 0; i < limits::kMaxDevices; i++) {
        devices.devices.push_back(haDevice("dev_x" + std::to_string(i), "X", "tv", "media_player.x"));
    }
    report = {};
    EXPECT_EQ(compile(devices), nullptr);
    EXPECT_TRUE(report.has(ErrorCode::TooManyDevices, "/devices"));
}

TEST_F(CompilerTest, ReportIsCapped) {
    ConfigSpec cfg = fixtureConfig();
    for (int i = 0; i < 50; i++) cfg.globalBindings.push_back(bind("bad" + std::to_string(i), {}));
    compile(cfg);
    EXPECT_EQ(report.issues().size(), limits::kMaxValidationIssues);
    EXPECT_EQ(report.dropped(), 50 - limits::kMaxValidationIssues);
}

TEST_F(CompilerTest, ValidateActivityIsRootedAtTheActivity) {
    const ConfigSpec context = fixtureConfig();
    ActivitySpec a = context.activities[0];
    EXPECT_TRUE(validateActivity(a, context, adapters).ok());

    a.id = "new_activity";
    a.bindings.push_back(bind("red:tap", {activateAction("new_activity")}));  // may reference itself
    a.bindings.push_back(bind("blue:tap", {bleKey("key_tap", "NOPE")}));
    const ValidationReport r = validateActivity(a, context, adapters);
    ASSERT_EQ(r.issues().size(), 1u);
    EXPECT_EQ(r.issues()[0].code, ErrorCode::InvalidEnumValue);
    EXPECT_EQ(r.issues()[0].path, "/bindings/blue:tap/0/params/key");
}

TEST_F(CompilerTest, FailedCompileProducesNothing) {
    ConfigSpec cfg = fixtureConfig();
    cfg.activities[1].bindings.push_back(bind("up:down", {bleKey("key_down", "NOPE")}));
    EXPECT_EQ(compile(cfg), nullptr);
    EXPECT_FALSE(report.ok());
}

// Turning an integration off is always allowed: its key_down/key_up holds
// still pair (the actions just don't run), and an unpaired one still fails.
TEST_F(CompilerTest, DisablingAnIntegrationKeepsItsHoldsPaired) {
    ConfigSpec c = fixtureConfig();
    for (auto &integration : c.integrations) {
        if (integration.id == "ble") integration.enabled = false;
    }
    auto compiled = compile(c);
    ASSERT_NE(compiled, nullptr);
    EXPECT_TRUE(report.ok()) << report.issues()[0].path;
    const ActionSpan down = compiled->lookup(0, Button::Up, EventKind::Down);
    ASSERT_EQ(down.count, 1);
    EXPECT_EQ(compiled->span(down)[0].op, ActionOp::Unavailable);

    ActivitySpec &watch = c.activities[0];
    for (auto &b : watch.bindings) {
        if (b.trigger == "up:up") b.actions.clear();
    }
    ValidationReport unpaired;
    EXPECT_EQ(compileConfig(c, adapters, &unpaired), nullptr);
    ASSERT_FALSE(unpaired.ok());
    EXPECT_EQ(unpaired.issues()[0].code, ErrorCode::UnpairedHold);
}

TEST(JsonPointer, Escapes) {
    EXPECT_EQ(jsonPointerToken("a/b~c"), "a~1b~0c");
    EXPECT_EQ(jsonPointerToken("up:down"), "up:down");
}
