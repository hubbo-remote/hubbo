// Smart buttons (docs/design/smart-buttons.md): the config's shape, the
// actions derived from a slot's device, compile and edits, and the engine's
// resolution, selection, release and tap merge.
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "fixtures.h"
#include "hub.h"
#include "hubbo/config/json.h"
#include "hubbo/engine/smart_actions.h"

using namespace hubbo;
using namespace hubbo::config;
using namespace hubbo::sim;
using remote::Button;

namespace {

using Lines = std::vector<std::string>;

// Light 1 on the fixture's lights (on, off, brightness +/-26) and Switch 1
// on a cover with a position (open, close, position +/-10, hold to move).
ConfigSpec withSlots() {
    ConfigSpec c = fixtureConfig();
    c.devices.push_back(haDevice("dev_blinds", "Office Blinds", "cover", "cover.office_blinds"));
    c.smartButtons.at(SmartSlot::Light1) = "dev_lights";
    c.smartButtons.at(SmartSlot::Switch1) = "dev_blinds";
    return c;
}

ParseResult parseText(const std::string &text, ConfigSpec *out) { return parseConfig(text, out); }

// The PUT body's issues.
ValidationReport decodeBody(const char *text, SmartButtonsSpec *out = nullptr) {
    JsonDocument doc;
    EXPECT_EQ(parseJson(text, doc, kMaxRequestBytes, kRequestNestingLimit), ErrorCode::Ok);
    ValidationReport report;
    SmartButtonsSpec spec;
    decodeSmartButtons(doc.as<JsonVariantConst>(), out != nullptr ? out : &spec, &report);
    return report;
}

class SmartButtonsEngine : public ::testing::Test {
protected:
    SmartButtonsEngine() { EXPECT_TRUE(hub.load(withSlots()).ok()); }

    Lines take() {
        Lines out;
        for (; seen < hub.commands().size(); seen++) {
            const std::string &line = hub.commands()[seen].text;
            out.push_back(line.substr(0, line.find(" -> ")));
        }
        return out;
    }
    // A press held past Hold for `ms` in all, then released.
    void hold(Button button, uint32_t ms) {
        hub.press(button);
        hub.advance(ms);
        hub.release(button);
        hub.advance(10);
    }

    SimHub hub;
    size_t seen = 0;
};

const char *kOn = "ha_main dev_lights light.turn_on";
const char *kOff = "ha_main dev_lights light.turn_off";
const char *kUp = "ha_main dev_lights light.brightness_step step=26";
const char *kDown = "ha_main dev_lights light.brightness_step step=-26";
const char *kCoverUp = "ha_main dev_blinds cover.position_step step=10";
const char *kCoverDown = "ha_main dev_blinds cover.position_step step=-10";
const char *kOpen = "ha_main dev_blinds cover.open_cover";
const char *kStop = "ha_main dev_blinds cover.stop_cover";

// A press as "tap / hold / release", each "command[:step],...".
std::string describe(const SmartPressActions &press) {
    const auto list = [](const std::vector<ActionSpec> &actions) {
        std::string out;
        for (const ActionSpec &a : actions) {
            if (!out.empty()) out += ",";
            out += a.commandId;
            for (const ParamInput &p : a.params) out += ":" + std::to_string(p.value.integer);
        }
        return out;
    };
    return list(press.tap) + " / " + list(press.hold) + " / " + list(press.release);
}

}  // namespace

// ---- config ---------------------------------------------------------------

TEST(SmartButtonsConfig, RoundTripsThroughTheConfigAndABackup) {
    const ConfigSpec c = withSlots();
    const std::string text = serializeConfig(c);
    ConfigSpec parsed;
    ASSERT_EQ(parseText(text, &parsed).code, ErrorCode::Ok);
    EXPECT_EQ(serializeConfig(parsed), text);
    EXPECT_EQ(parsed.smartButtons.at(SmartSlot::Light1), std::optional<std::string>("dev_lights"));
    EXPECT_FALSE(parsed.smartButtons.at(SmartSlot::Light2));
    EXPECT_NE(text.find(R"("smart_buttons":{"revision":1,"slots":{"light_1":"dev_lights","light_2":null,)"
                        R"("switch_1":"dev_blinds","switch_2":null}})"),
              std::string::npos);

    ConfigSpec restored;
    ASSERT_EQ(parseBackup(serializeBackup(c, BackupInfo{}), &restored).code, ErrorCode::Ok);
    EXPECT_EQ(restored.smartButtons.at(SmartSlot::Switch1), std::optional<std::string>("dev_blinds"));
}

// The one tolerance: configs and backups saved before smart buttons.
TEST(SmartButtonsConfig, AnAbsentSectionIsFourEmptySlots) {
    ConfigSpec c = fixtureConfig();
    c.revision = 7;
    JsonDocument doc;
    deserializeJson(doc, serializeConfig(c));
    doc.remove("smart_buttons");
    std::string text;
    serializeJson(doc, text);
    ConfigSpec parsed;
    ASSERT_EQ(parseText(text, &parsed).code, ErrorCode::Ok);
    EXPECT_EQ(parsed.smartButtons.revision, 7u);  // the config's: never 0
    ASSERT_EQ(parsed.smartButtons.slots.size(), kSmartSlotCount);
    for (const auto &slot : parsed.smartButtons.slots) EXPECT_FALSE(slot);

    JsonDocument backup;
    deserializeJson(backup, serializeBackup(c, BackupInfo{}));
    backup.remove("smart_buttons");
    text.clear();
    serializeJson(backup, text);
    EXPECT_EQ(parseBackup(text, &parsed).code, ErrorCode::Ok);
}

TEST(SmartButtonsConfig, TheBodyIsStrict) {
    SmartButtonsSpec spec;
    EXPECT_TRUE(decodeBody(R"({"revision":3,"slots":{"light_1":"dev_lights","light_2":null,"switch_1":null,)"
                           R"("switch_2":null}})",
                           &spec)
                    .ok());
    EXPECT_EQ(spec.revision, 3u);
    EXPECT_EQ(spec.at(SmartSlot::Light1), std::optional<std::string>("dev_lights"));

    // A missing slot, an extra one, and the revision.
    const ValidationReport r =
        decodeBody(R"({"slots":{"light_1":null,"light_2":null,"switch_1":null,"switch_3":null}})");
    EXPECT_TRUE(r.has(ErrorCode::Required, "/slots/switch_2"));
    EXPECT_TRUE(r.has(ErrorCode::UnknownField, "/slots/switch_3"));
    EXPECT_TRUE(r.has(ErrorCode::Required, "/revision"));

    // A slot is a device id or null: the old slot object is just invalid.
    const ValidationReport b = decodeBody(R"({"revision":3,"slots":{"light_1":{"device_id":null},"light_2":7,)"
                                          R"("switch_1":null,"switch_2":null}})");
    EXPECT_TRUE(b.has(ErrorCode::WrongType, "/slots/light_1"));
    EXPECT_TRUE(b.has(ErrorCode::WrongType, "/slots/light_2"));
}

TEST(SmartButtonsConfig, CompileDerivesTheSlots) {
    FakeAdapters adapters;
    ConfigSpec c = withSlots();
    c.smartButtons.at(SmartSlot::Light2) = "dev_gone";
    c.smartButtons.at(SmartSlot::Switch2) = "Not An Id";
    ValidationReport r;
    EXPECT_EQ(compileConfig(c, adapters.set, &r), nullptr);
    EXPECT_TRUE(r.has(ErrorCode::UnknownDevice, "/smart_buttons/slots/light_2"));
    EXPECT_TRUE(r.has(ErrorCode::InvalidId, "/smart_buttons/slots/switch_2"));

    c = withSlots();
    c.smartButtons.at(SmartSlot::Light2) = "dev_denon";  // a receiver: no actions
    ValidationReport ok;
    const auto compiled = compileConfig(c, adapters.set, &ok);
    ASSERT_NE(compiled, nullptr);
    const CompiledSmartSlot &light = compiled->smartSlots[0];
    EXPECT_EQ(light.at(SmartPress::Button).tap.count, 1);
    EXPECT_EQ(light.at(SmartPress::Button).hold.count, 1);
    EXPECT_EQ(light.at(SmartPress::Plus).tap.count, 1);
    EXPECT_EQ(light.at(SmartPress::Plus).hold.count, 0);
    EXPECT_FALSE(compiled->smartSlots[1].at(SmartPress::Button).set());
    EXPECT_FALSE(compiled->smartSlots[1].at(SmartPress::Plus).set());
    const CompiledSmartSlot &cover = compiled->smartSlots[2];
    EXPECT_EQ(cover.at(SmartPress::Minus).release.count, 1);
    EXPECT_FALSE(compiled->smartSlots[3].at(SmartPress::Button).set());
}

TEST(SmartButtonsConfig, ActionsComeFromTheDeviceKindAndItsCommands) {
    static const CommandSpec kAll[] = {
        {"light.turn_on", nullptr, 0},       {"light.turn_off", nullptr, 0},     {"light.brightness_step", nullptr, 0},
        {"fan.turn_on", nullptr, 0},         {"fan.turn_off", nullptr, 0},       {"fan.increase_speed", nullptr, 0},
        {"fan.decrease_speed", nullptr, 0},  {"switch.turn_on", nullptr, 0},     {"switch.turn_off", nullptr, 0},
        {"cover.open_cover", nullptr, 0},    {"cover.close_cover", nullptr, 0},  {"cover.stop_cover", nullptr, 0},
        {"cover.position_step", nullptr, 0},
    };
    const CommandCatalog all{kAll, static_cast<uint16_t>(std::size(kAll))};
    const auto slot = [&](const char *kind, const char *entity, const CommandCatalog &catalog = {}) {
        DeviceSpec d = haDevice("dev_x", "X", kind, entity);
        const SmartSlotActions a = smartSlotActions(d, catalog.count != 0 ? catalog : all);
        return describe(a.button) + " | " + describe(a.plus) + " | " + describe(a.minus);
    };

    EXPECT_EQ(slot("light", "light.desk"), "light.turn_on / light.turn_off /  | light.brightness_step:26 /  /  | "
                                           "light.brightness_step:-26 /  / ");
    EXPECT_EQ(slot("fan", "fan.ceiling"), "fan.turn_on / fan.turn_off /  | fan.increase_speed /  /  | "
                                          "fan.decrease_speed /  / ");
    EXPECT_EQ(slot("switch", "switch.porch"), "switch.turn_on / switch.turn_off /  |  /  /  |  /  / ");
    EXPECT_EQ(slot("cover", "cover.blinds"),
              "cover.open_cover / cover.close_cover /  | cover.position_step:10 / cover.open_cover / cover.stop_cover"
              " | cover.position_step:-10 / cover.close_cover / cover.stop_cover");

    // Only what the device offers: no brightness, no position.
    static const CommandSpec kPlain[] = {{"light.turn_on", nullptr, 0}, {"light.turn_off", nullptr, 0},
                                         {"cover.open_cover", nullptr, 0}, {"cover.close_cover", nullptr, 0}};
    const CommandCatalog plain{kPlain, static_cast<uint16_t>(std::size(kPlain))};
    EXPECT_EQ(slot("light", "light.desk", plain), "light.turn_on / light.turn_off /  |  /  /  |  /  / ");
    EXPECT_EQ(slot("cover", "cover.blinds", plain),
              "cover.open_cover / cover.close_cover /  | cover.open_cover /  /  | cover.close_cover /  / ");

    // The entity's domain, not the kind, names the service; another kind gets nothing.
    EXPECT_EQ(slot("receiver", "media_player.denon"), " /  /  |  /  /  |  /  / ");
    DeviceSpec ble = bleDevice("dev_ble", "Remote", "light");  // a light kind, but not Home Assistant
    const SmartSlotActions none = smartSlotActions(ble, all);
    EXPECT_TRUE(none.button.tap.empty() && none.plus.tap.empty());
    const SmartSlotActions a = smartSlotActions(haDevice("dev_x", "X", "light", "light.desk"), all);
    ASSERT_EQ(a.button.tap.size(), 1u);
    EXPECT_EQ(a.button.tap[0].integrationId, "ha_main");
    EXPECT_EQ(a.button.tap[0].deviceId, std::optional<std::string>("dev_x"));
}

TEST(SmartButtonsConfig, ReplaceHasItsOwnRevisionAndBodyPaths) {
    SimHub hub;
    ASSERT_TRUE(hub.load(withSlots()).ok());
    ConfigService &service = hub.configService();
    const uint32_t base = service.config().smartButtons.revision;
    const uint32_t bindings = service.config().globalBindingsRevision;

    SmartButtonsSpec next = service.config().smartButtons;
    next.at(SmartSlot::Light2) = "dev_lights";
    SmartButtonsSpec stale = next;
    EXPECT_EQ(service.replaceSmartButtons(std::move(stale), base + 1).code, ErrorCode::RevisionConflict);

    Outcome done = service.replaceSmartButtons(SmartButtonsSpec(next), base);
    ASSERT_TRUE(done.ok());
    EXPECT_EQ(service.config().smartButtons.revision, done.revision);
    EXPECT_GT(done.revision, base);
    EXPECT_EQ(service.config().globalBindingsRevision, bindings);  // untouched
    EXPECT_EQ(service.config().smartButtons.at(SmartSlot::Light2), std::optional<std::string>("dev_lights"));

    next.at(SmartSlot::Light2) = "dev_gone";
    const Outcome refused = service.replaceSmartButtons(std::move(next), done.revision);
    EXPECT_EQ(refused.code, ErrorCode::ValidationFailed);
    EXPECT_TRUE(refused.issues.has(ErrorCode::UnknownDevice, "/slots/light_2"));
}

TEST(SmartButtonsConfig, ASlotKeepsItsDevice) {
    SimHub hub;
    ConfigSpec c = withSlots();
    c.smartButtons.at(SmartSlot::Light2) = "dev_denon";
    ASSERT_TRUE(hub.load(c).ok());
    ConfigService &service = hub.configService();

    const std::vector<Referrer> blinds = service.deviceReferrers("dev_blinds");
    ASSERT_EQ(blinds.size(), 1u);
    EXPECT_EQ(blinds[0].owner, ReferrerOwner::SmartButtons);
    EXPECT_EQ(blinds[0].path, "/slots/switch_1");
    EXPECT_TRUE(blinds[0].activityId.empty());

    const DeviceSpec *denon = service.findDevice("dev_denon");
    const Outcome inUse = service.deleteDevice("dev_denon", denon->revision);
    EXPECT_EQ(inUse.code, ErrorCode::DeviceInUse);
    EXPECT_TRUE(std::any_of(inUse.referrers.begin(), inUse.referrers.end(), [](const Referrer &r) {
        return r.owner == ReferrerOwner::SmartButtons && r.path == "/slots/light_2";
    }));
}

// ---- engine ---------------------------------------------------------------

TEST_F(SmartButtonsEngine, TheSlotOwnsItsButtonUnlessTheActivityBindsIt) {
    hub.tap(Button::Light1);
    hub.advance(10);
    EXPECT_EQ(take(), (Lines{kOn}));  // not the global light_1:down too
    hold(Button::Light1, 900);
    EXPECT_EQ(take(), (Lines{kOff}));

    // An empty slot leaves the button to the global bindings.
    hub.tap(Button::Light2);
    EXPECT_EQ(take(), (Lines{kOff}));

    // An activity that binds the button wins, whatever the slot holds.
    ConfigSpec c = withSlots();
    c.activities[0].bindings.push_back(bind("light_1:down", {haAction("dev_denon", "media_player.volume_up")}));
    ASSERT_TRUE(hub.load(c).ok());
    hub.activate("act_watch_apple_tv");
    hub.advance(2000);
    take();
    hub.tap(Button::Light1);
    EXPECT_EQ(take(), (Lines{"ha_main dev_denon media_player.volume_up"}));
}

TEST_F(SmartButtonsEngine, PlusAndMinusFollowTheSelectedSlot) {
    EXPECT_EQ(hub.engine().selectedSlotShared(), SmartSlot::Light1);  // at boot
    hub.tap(Button::Plus);
    hub.tap(Button::Minus);  // sends the pending +
    hub.advance(kSmartStepWindowMs + 10);
    EXPECT_EQ(take(), (Lines{kUp, kDown}));

    hub.tap(Button::Switch1);
    take();
    EXPECT_EQ(hub.engine().selectedSlotShared(), SmartSlot::Switch1);
    hub.tap(Button::Plus);
    hub.tap(Button::Minus);
    hub.advance(kSmartStepWindowMs + 10);
    EXPECT_EQ(take(), (Lines{kCoverUp, kCoverDown}));

    // A slot button the activity binds doesn't select.
    ConfigSpec c = withSlots();
    c.activities[0].bindings.push_back(bind("light_1:tap", {haAction("dev_denon", "media_player.volume_up")}));
    ASSERT_TRUE(hub.load(c).ok());
    hub.activate("act_watch_apple_tv");
    hub.advance(2000);
    hub.tap(Button::Light1);
    EXPECT_EQ(hub.engine().selectedSlotShared(), SmartSlot::Switch1);

    // An empty slot button still selects; its +/- fall back to the global
    // bindings (none here).
    hub.tap(Button::Switch2);
    EXPECT_EQ(hub.engine().selectedSlotShared(), SmartSlot::Switch2);
    take();
    hub.tap(Button::Plus);
    hub.advance(kSmartStepWindowMs + 10);
    EXPECT_TRUE(take().empty());
}

// A hold fires once, and its release follows the Up.
TEST_F(SmartButtonsEngine, ReleaseRunsOnlyAfterAHold) {
    hub.tap(Button::Switch1);
    take();
    hub.tap(Button::Plus);
    hub.advance(kSmartStepWindowMs + 10);
    EXPECT_EQ(take(), (Lines{kCoverUp}));  // no release after a tap
    hold(Button::Plus, 3000);
    EXPECT_EQ(take(), (Lines{kOpen, kStop}));

    // A lost release (radio silence) is an Up after the Hold too.
    hub.press(Button::Plus);
    hub.advance(900);
    hub.advance(600, /*silent=*/true);
    EXPECT_EQ(take(), (Lines{kOpen, kStop}));
}

// + steps the position on a tap (the scripted fake keeps the position it
// was sent), opens while held and stops on the release.
TEST_F(SmartButtonsEngine, ACoverStepsAndStops) {
    hub.tap(Button::Switch1);
    std::vector<std::string> &calls = hub.homeAssistant().calls();
    calls.clear();
    hub.tap(Button::Plus);
    hub.tap(Button::Plus);
    hold(Button::Plus, 1000);  // the Hold sends the two taps as one step of 20
    EXPECT_EQ(calls, (Lines{"cover.set_cover_position cover.office_blinds position=70",
                            "cover.open_cover cover.office_blinds", "cover.stop_cover cover.office_blinds"}));
}

TEST_F(SmartButtonsEngine, AnActivitySwitchMidHoldStillStopsTheCover) {
    hub.tap(Button::Switch1);
    take();
    hub.press(Button::Plus);
    hub.advance(900);
    hub.activate("act_watch_apple_tv");
    take();
    hub.release(Button::Plus);
    hub.advance(10);
    EXPECT_EQ(take(), (Lines{kStop}));
}

TEST_F(SmartButtonsEngine, AnotherSlotPressedMidHoldDoesNotStealTheRelease) {
    hub.tap(Button::Switch1);
    take();
    hub.press(Button::Plus);
    hub.advance(900);
    hub.tap(Button::Light1);  // selects light_1 while + is down
    EXPECT_EQ(hub.engine().selectedSlotShared(), SmartSlot::Light1);
    hub.release(Button::Plus);
    hub.advance(10);
    EXPECT_EQ(take(), (Lines{kOpen, kOn, kStop}));
}

TEST_F(SmartButtonsEngine, AConfigSaveMidHoldStillStopsTheCover) {
    hub.tap(Button::Switch1);
    take();
    hub.press(Button::Plus);
    hub.advance(900);
    ConfigSpec c = withSlots();
    c.smartButtons.at(SmartSlot::Light2) = "dev_lights";  // any edit
    ASSERT_TRUE(hub.load(c).ok());
    hub.release(Button::Plus);
    hub.advance(10);
    EXPECT_EQ(take(), (Lines{kOpen, kStop}));
}

// ---- the tap merge -------------------------------------------------------

namespace {

// A fan (no `step`: its + and - never merge) on Light 2.
ConfigSpec withFan() {
    ConfigSpec c = withSlots();
    DeviceSpec fan = haDevice("dev_fan", "Ceiling Fan", "fan", "fan.ceiling");
    ha::CapabilitySnapshot snapshot;
    snapshot.platform = "demo";
    snapshot.supportedFeatures = ha::fan_feature::kSetSpeed | ha::fan_feature::kTurnOn | ha::fan_feature::kTurnOff;
    fan.haCapabilities = snapshot;
    c.devices.push_back(std::move(fan));
    c.smartButtons.at(SmartSlot::Light2) = "dev_fan";
    return c;
}

}  // namespace

TEST_F(SmartButtonsEngine, QuickTapsGoAsOneStepAfterTheWindow) {
    hub.tap(Button::Plus);
    hub.tap(Button::Plus);
    hub.tap(Button::Plus);
    hub.advance(kSmartStepWindowMs - 10);
    EXPECT_TRUE(take().empty());  // the window runs from the last tap
    hub.advance(20);
    EXPECT_EQ(take(), (Lines{"ha_main dev_lights light.brightness_step step=78"}));

    // One tap alone is the action as it is.
    hub.tap(Button::Minus);
    hub.advance(kSmartStepWindowMs + 10);
    EXPECT_EQ(take(), (Lines{kDown}));
}

TEST_F(SmartButtonsEngine, TheMergedStepIsClampedToTheParamsRange) {
    for (int i = 0; i < 10; i++) hub.tap(Button::Plus);  // 260
    hub.advance(kSmartStepWindowMs + 10);
    EXPECT_EQ(take(), (Lines{"ha_main dev_lights light.brightness_step step=255"}));

    hub.tap(Button::Switch1);
    take();
    for (int i = 0; i < 11; i++) hub.tap(Button::Minus);  // -110
    hub.advance(kSmartStepWindowMs + 10);
    EXPECT_EQ(take(), (Lines{"ha_main dev_blinds cover.position_step step=-100"}));
}

TEST_F(SmartButtonsEngine, TheSameButtonDownPausesTheWindowUntilItsUp) {
    hub.tap(Button::Plus);
    hub.advance(100);
    hub.press(Button::Plus);
    hub.advance(500);  // past the window, short of a Hold
    EXPECT_TRUE(take().empty());
    hub.release(Button::Plus);
    hub.advance(kSmartStepWindowMs - 10);
    EXPECT_TRUE(take().empty());  // restarted at its Up
    hub.advance(20);
    EXPECT_EQ(take(), (Lines{"ha_main dev_lights light.brightness_step step=52"}));
}

TEST_F(SmartButtonsEngine, PendingTapsAreSentAtOnce) {
    // A tap the other way: the pending + goes, the - starts its own.
    hub.tap(Button::Plus);
    hub.tap(Button::Plus);
    hub.tap(Button::Minus);
    EXPECT_EQ(take(), (Lines{"ha_main dev_lights light.brightness_step step=52"}));
    hub.advance(kSmartStepWindowMs + 10);
    EXPECT_EQ(take(), (Lines{kDown}));

    // A slot change, before the new slot's own button runs.
    hub.tap(Button::Plus);
    hub.tap(Button::Switch1);
    EXPECT_EQ(take(), (Lines{kUp, "ha_main dev_blinds cover.open_cover"}));

    // A Hold on + or -, before the hold runs.
    hub.tap(Button::Plus);
    hub.press(Button::Minus);
    hub.advance(900);
    EXPECT_EQ(take(), (Lines{kCoverUp, "ha_main dev_blinds cover.close_cover"}));
    hub.release(Button::Minus);
    hub.advance(10);
    take();

    // An activity change, and off.
    hub.tap(Button::Light1);
    take();
    hub.tap(Button::Plus);
    ASSERT_EQ(hub.activate("act_watch_apple_tv"), ErrorCode::Ok);
    Lines lines = take();
    EXPECT_NE(std::find(lines.begin(), lines.end(), kUp), lines.end());
    hub.advance(3000);
    take();
    hub.tap(Button::Plus);
    ASSERT_EQ(hub.off(), ErrorCode::Ok);
    lines = take();
    EXPECT_NE(std::find(lines.begin(), lines.end(), kUp), lines.end());
    hub.advance(3000);
    take();
    hub.advance(kSmartStepWindowMs + 10);
    EXPECT_TRUE(take().empty());  // nothing left over
}

TEST_F(SmartButtonsEngine, TapsWithoutAStepAreInstant) {
    ASSERT_TRUE(hub.load(withFan()).ok());
    hub.tap(Button::Light2);
    take();
    hub.tap(Button::Plus);
    EXPECT_EQ(take(), (Lines{"ha_main dev_fan fan.increase_speed"}));
    hub.tap(Button::Plus);
    hub.tap(Button::Minus);
    EXPECT_EQ(take(), (Lines{"ha_main dev_fan fan.increase_speed", "ha_main dev_fan fan.decrease_speed"}));
}

// A config save sends pending taps at once, as tapped: never with what
// the slot holds after the save.
TEST_F(SmartButtonsEngine, AConfigSaveSendsPendingTapsAsTapped) {
    hub.tap(Button::Plus);
    hub.tap(Button::Plus);
    ConfigSpec c = withSlots();
    c.smartButtons.at(SmartSlot::Light1) = "dev_blinds";  // the light slot is now a cover
    ASSERT_TRUE(hub.load(c).ok());
    hub.advance(1);
    EXPECT_EQ(take(), (Lines{"ha_main dev_lights light.brightness_step step=52"}));
    hub.advance(kSmartStepWindowMs + 10);
    EXPECT_TRUE(take().empty());

    // The next taps are the new device's.
    hub.tap(Button::Plus);
    hub.advance(kSmartStepWindowMs + 10);
    EXPECT_EQ(take(), (Lines{kCoverUp}));
}

// Present, the section's revision is required in the config file, as
// global_bindings'; only a backup may leave it out.
TEST(SmartButtonsConfig, ThePresentSectionNeedsItsRevisionInTheConfigFile) {
    const ConfigSpec c = withSlots();
    JsonDocument doc;
    deserializeJson(doc, serializeConfig(c));
    doc["smart_buttons"].remove("revision");
    std::string text;
    serializeJson(doc, text);
    ConfigSpec parsed;
    const ParseResult result = parseText(text, &parsed);
    EXPECT_EQ(result.code, ErrorCode::ValidationFailed);
    EXPECT_TRUE(result.report.has(ErrorCode::Required, "/smart_buttons/revision"));

    JsonDocument backup;
    deserializeJson(backup, serializeBackup(c, BackupInfo{}));
    backup["smart_buttons"].remove("revision");
    text.clear();
    serializeJson(backup, text);
    EXPECT_EQ(parseBackup(text, &parsed).code, ErrorCode::Ok);
}
