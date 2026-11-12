// Design sections 3 and 4: the integration resolver (existence, type,
// enabled, adapter), command effects with hold/release pairing, and
// transitions that report each phase.
#include <gtest/gtest.h>

#include <algorithm>

#include "fakes.h"
#include "fixtures.h"
#include "hub.h"
#include "hubbo/engine/compiler.h"

using namespace hubbo;
using namespace hubbo::sim;
using remote::Button;

namespace {

ValidationReport compileWith(const ConfigSpec &cfg) {
    FakeAdapters fakes;
    ValidationReport r;
    compileConfig(cfg, fakes.set, &r);
    return r;
}

ConfigSpec withBindings(std::vector<BindingSpec> bindings) {
    ConfigSpec cfg = fixtureConfig();
    cfg.activities[0].bindings = std::move(bindings);
    return cfg;
}

}  // namespace

// ---- hold / release pairing ------------------------------------------------

TEST(HoldPairing, ADownHoldNeedsTheMatchingUpRelease) {
    EXPECT_TRUE(compileWith(withBindings({bind("up:down", {bleKey("key_down", "ARROW_UP")}),
                                          bind("up:up", {bleKey("key_up", "ARROW_UP")})}))
                    .ok());
    EXPECT_TRUE(compileWith(withBindings({bind("up:down", {bleKey("key_down", "ARROW_UP")})}))
                    .has(ErrorCode::UnpairedHold, "/activities/0/bindings/up:down/0/command_id"));
    // A release of another key doesn't count.
    EXPECT_TRUE(compileWith(withBindings({bind("up:down", {bleKey("key_down", "ARROW_UP")}),
                                          bind("up:up", {bleKey("key_up", "ARROW_DOWN")})}))
                    .has(ErrorCode::UnpairedHold, "/activities/0/bindings/up:down/0/command_id"));
}

TEST(HoldPairing, HoldsOnlyInDownBindings) {
    EXPECT_TRUE(compileWith(withBindings({bind("up:tap", {bleKey("key_down", "ARROW_UP")})}))
                    .has(ErrorCode::HoldNotAllowed, "/activities/0/bindings/up:tap/0/command_id"));
    ConfigSpec cfg = fixtureConfig();
    cfg.activities[0].startup.push_back(bleKey("key_down", "SELECT"));
    EXPECT_TRUE(compileWith(cfg).has(ErrorCode::HoldNotAllowed, "/activities/0/startup/5/command_id"));
    // Global bindings follow the same rule.
    cfg = fixtureConfig();
    cfg.globalBindings.push_back(bind("red:down", {bleKey("key_down", "F2")}));
    EXPECT_TRUE(compileWith(cfg).has(ErrorCode::UnpairedHold, "/global_bindings/red:down/0/command_id"));
}

TEST(HoldPairing, CatalogDeclaresEffects) {
    FakeAdapters fakes;
    const CommandCatalog c = fakes.ble.commandsFor(bleDevice("dev_apple_tv", "Apple TV", "streamer"));
    EXPECT_EQ(c.items[c.find("key_down")].effect, CommandEffect::Hold);
    EXPECT_STREQ(c.items[c.find("key_down")].releasedBy, "key_up");
    EXPECT_EQ(c.items[c.find("key_up")].effect, CommandEffect::Momentary);
    EXPECT_EQ(c.items[c.find("key_tap")].effect, CommandEffect::Momentary);
    EXPECT_STREQ(commandEffectName(CommandEffect::Hold), "hold");
    EXPECT_STREQ(commandEffectName(CommandEffect::Wait), "wait");
}

TEST(HoldPairing, DirectExecutionIsMomentaryOnly) {
    SimHub hub;
    hub.load(fixtureConfig());
    CompiledCommand cmd;
    const config::Outcome hold = hub.configService().compileCommand("dev_apple_tv", "key_down", {param("key", "SELECT")}, &cmd);
    EXPECT_EQ(hold.code, ErrorCode::ValidationFailed);
    EXPECT_TRUE(hold.issues.has(ErrorCode::HoldNotAllowed, "/command_id"));
    EXPECT_TRUE(hub.configService().compileCommand("dev_apple_tv", "key_tap", {param("key", "SELECT")}, &cmd).ok());
    // A wait is for startups and shutdowns.
    const config::Outcome wait = hub.configService().compileCommand("dev_apple_tv", "wait_connected", {}, &cmd);
    EXPECT_EQ(wait.code, ErrorCode::ValidationFailed);
    EXPECT_TRUE(wait.issues.has(ErrorCode::SequenceOnly, "/command_id"));
}

// wait_connected runs in a startup or shutdown, never in a binding.
TEST(WaitConnected, SequencesOnly) {
    ConfigSpec c = fixtureConfig();
    FakeAdapters fakes;
    const ActionSpec wait = action("ble", "dev_apple_tv", "wait_connected", {});
    c.activities[0].startup.insert(c.activities[0].startup.begin(), wait);
    c.activities[0].shutdown.push_back(wait);
    ValidationReport ok;
    EXPECT_NE(compileConfig(c, fakes.set, &ok), nullptr) << (ok.ok() ? "" : ok.issues()[0].path);
    c.activities[0].bindings.push_back(bind("red:tap", {wait}));
    ValidationReport report;
    EXPECT_EQ(compileConfig(c, fakes.set, &report), nullptr);
    EXPECT_TRUE(report.has(ErrorCode::SequenceOnly, "/activities/0/bindings/red:tap/0/command_id"));
}

// ---- the integration resolver ------------------------------------------------

TEST(Resolver, ChecksExistenceTypeAndAdapter) {
    FakeAdapters fakes;
    ConfigSpec cfg = fixtureConfig();
    const DeviceSpec tv = *std::find_if(cfg.devices.begin(), cfg.devices.end(), [](const DeviceSpec &d) {
        return d.id == "dev_lg_tv";
    });
    EXPECT_EQ(resolveIntegration(cfg, fakes.set, tv).error, ErrorCode::Ok);

    DeviceSpec wrongType = tv;
    wrongType.integrationId = "ble";  // a home_assistant provider on the BLE integration
    EXPECT_EQ(resolveIntegration(cfg, fakes.set, wrongType).error, ErrorCode::IntegrationMismatch);

    DeviceSpec unlisted = tv;
    unlisted.integrationId = "ha_other";
    EXPECT_EQ(resolveIntegration(cfg, fakes.set, unlisted).error, ErrorCode::UnknownIntegration);

    // Listed, but no adapter registered for it.
    cfg.integrations.push_back(integration("ha_other", "home_assistant", "Other HA"));
    EXPECT_EQ(resolveIntegration(cfg, fakes.set, unlisted).error, ErrorCode::AdapterUnavailable);

    // Listed with a type the registered adapter doesn't implement.
    for (auto &i : cfg.integrations) {
        if (i.id == "ha_main") i.type = "ble_hid";
    }
    DeviceSpec bleOnHa = tv;
    bleOnHa.provider = {"ble_hid", "", ""};
    EXPECT_EQ(resolveIntegration(cfg, fakes.set, bleOnHa).error, ErrorCode::IntegrationMismatch);
}

TEST(Resolver, ADisabledIntegrationReportsInsteadOfReachingTheAdapter) {
    SimHub hub;
    ConfigSpec cfg = fixtureConfig();
    for (auto &i : cfg.integrations) {
        if (i.id == "ha_main") i.enabled = false;
    }
    ASSERT_TRUE(hub.load(cfg).ok());
    const size_t callsBefore = hub.homeAssistant().calls().size();
    hub.activate("act_watch_apple_tv");
    hub.advance(1500);
    EXPECT_EQ(hub.homeAssistant().calls().size(), callsBefore);  // HA never called
    ASSERT_FALSE(hub.commands().empty());
    EXPECT_EQ(hub.commands().front().text, "ha_main dev_lg_tv media_player.turn_on -> failed integration_disabled");
    hub.press(Button::Up);  // the BLE parts still work
    EXPECT_EQ(hub.commands().back().text, "ble dev_apple_tv key_down key=ARROW_UP -> accepted");
    hub.release(Button::Up);

    CompiledCommand cmd;
    EXPECT_EQ(hub.configService().compileCommand("dev_denon", "media_player.volume_up", {}, &cmd).code,
              ErrorCode::IntegrationDisabled);
}

TEST(Resolver, BuiltinIntegrationsAreAlwaysThere) {
    SimHub hub;
    ConfigSpec bare = fixtureConfig();
    bare.integrations.erase(bare.integrations.begin(), bare.integrations.begin() + 2);  // drop system, ble
    ASSERT_TRUE(hub.load(bare).ok());
    EXPECT_NE(hub.configService().findIntegration("system"), nullptr);
    EXPECT_NE(hub.configService().findIntegration("ble"), nullptr);
}

// ---- transitions report each phase -------------------------------------------

TEST(TransitionPhases, ActivateAndOff) {
    SimHub hub;
    hub.load(fixtureConfig());
    TransitionResult r;
    ASSERT_EQ(hub.engine().activate("act_watch_apple_tv", hub.nowMs(), &r), ErrorCode::Ok);
    EXPECT_TRUE(r.switched);
    EXPECT_EQ(r.startup.status, PhaseStatus::Queued);
    EXPECT_EQ(r.startup.actions, 5);
    EXPECT_EQ(r.shutdown.status, PhaseStatus::None);
    ASSERT_EQ(hub.engine().off(hub.nowMs(), &r), ErrorCode::Ok);
    EXPECT_TRUE(r.switched);
    EXPECT_EQ(r.shutdown.status, PhaseStatus::Queued);
    EXPECT_EQ(r.previousActivityId, "act_watch_apple_tv");
    EXPECT_EQ(hub.engine().off(hub.nowMs(), &r), ErrorCode::NoActiveActivity);
    EXPECT_FALSE(r.switched);
}
