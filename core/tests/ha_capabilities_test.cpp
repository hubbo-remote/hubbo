#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "hubbo/domain/params.h"
#include "hubbo/ha/capabilities.h"

using namespace hubbo;
using namespace hubbo::ha;

namespace {

// Same entities as api/tools/gen_fixtures.py (HA_ENTITIES); the masks are the
// real integrations' supported_features (see docs/ha-capabilities.md).
EntityInfo denon() {
    EntityInfo e;
    e.entityId = "media_player.family_room_denon";
    e.supportedFeatures = 69004;
    e.sourceList = {"Blu-ray", "CBL/SAT", "DVD", "Game", "Media Player", "TV Audio", "Tuner", "Bluetooth"};
    e.soundModeList = {"MUSIC", "MOVIE", "GAME", "AUTO", "STEREO", "DIRECT"};
    return e;
}

EntityInfo sonos() {
    EntityInfo e;
    e.entityId = "media_player.family_room_sonos";
    e.supportedFeatures = 8321599;
    e.sourceList = {"TV"};
    return e;
}

EntityInfo lgTv() {
    EntityInfo e;
    e.entityId = "media_player.family_room_lg_tv";
    e.supportedFeatures = 24505;
    e.sourceList = {"HDMI 1", "HDMI 2", "HDMI 3", "Live TV", "Netflix", "YouTube"};
    return e;
}

EntityInfo xbox() {
    EntityInfo e;
    e.entityId = "media_player.xbox";
    e.supportedFeatures = 149433;
    return e;
}

EntityInfo hue() {
    EntityInfo e;
    e.entityId = "light.family_room";
    e.supportedFeatures = 44;
    e.supportedColorModes = {"color_temp", "xy"};
    e.minColorTempKelvin = 2000;
    e.maxColorTempKelvin = 6535;
    return e;
}

// Typical fans (supported_features and attributes as HA reports them).
EntityInfo speedFan() {  // SET_SPEED | TURN_OFF | TURN_ON, 3 speeds, preset_modes null
    EntityInfo e;
    e.entityId = "fan.ceiling_fan";
    e.supportedFeatures = 49;
    e.percentageStep = 33.333333333333336;
    return e;
}

EntityInfo presetFan() {  // + PRESET_MODE
    EntityInfo e;
    e.entityId = "fan.air_purifier";
    e.supportedFeatures = 57;
    e.presetModes = {"sleep"};
    e.percentageStep = 33.333333333333336;
    return e;
}

EntityInfo directionFan() {  // + DIRECTION, 7 speeds
    EntityInfo e;
    e.entityId = "fan.box_fan";
    e.supportedFeatures = 61;
    e.presetModes = {"auto"};
    e.percentageStep = 14.285714285714286;
    return e;
}

// One made-up entity per newer domain (as api/tools/gen_fixtures.py).
EntityInfo blinds() {  // OPEN | CLOSE | SET_POSITION | STOP
    EntityInfo e;
    e.entityId = "cover.office_blinds";
    e.supportedFeatures = 15;
    return e;
}

EntityInfo humidifierEntity() {  // MODES
    EntityInfo e;
    e.entityId = "humidifier.bedroom_humidifier";
    e.supportedFeatures = 1;
    e.availableModes = {"normal", "eco", "sleep"};
    e.minHumidity = 30;
    e.maxHumidity = 80;
    return e;
}

EntityInfo dryerCycle() {
    EntityInfo e;
    e.entityId = "select.dryer_cycle";
    e.options = {"normal", "delicate", "towels"};
    return e;
}

EntityInfo plain(const char *entityId) {
    EntityInfo e;
    e.entityId = entityId;
    return e;
}

std::vector<std::string> fans(std::initializer_list<const char *> names) {
    std::vector<std::string> out;
    for (const char *n : names) out.push_back(std::string("fan.") + n);
    return out;
}

std::vector<std::string> ids(const EntityCapabilities &caps) {
    std::vector<std::string> out;
    const CommandCatalog c = caps.catalog();
    for (uint16_t i = 0; i < c.count; i++) out.push_back(c.items[i].id);
    return out;
}

std::vector<std::string> mp(std::initializer_list<const char *> names) {
    std::vector<std::string> out;
    for (const char *n : names) out.push_back(std::string("media_player.") + n);
    return out;
}

const CommandSpec &command(const EntityCapabilities &caps, const char *id) {
    const CommandCatalog c = caps.catalog();
    const int index = c.find(id);
    EXPECT_GE(index, 0) << id;
    return c.items[index < 0 ? 0 : index];
}

ParamInput in(const char *name, ParamLiteral value) { return {name, std::move(value)}; }

// Resolve like the core does, then build the HA call.
ErrorCode call(const EntityCapabilities &caps, const char *id, const std::vector<ParamInput> &inputs,
               ServiceCall *out, std::string *bad = nullptr) {
    const CommandCatalog c = caps.catalog();
    const int index = c.find(id);
    if (index < 0) return ErrorCode::UnknownCommand;
    ParamValue values[limits::kMaxParams];
    std::string badParam;
    uint8_t present = 0;
    const ErrorCode code = resolveParams(c.items[index], inputs, values, &badParam, nullptr, &present);
    if (bad) *bad = badParam;
    if (code != ErrorCode::Ok) return code;
    return caps.buildServiceCall(static_cast<uint16_t>(index), values, present, out);
}

}  // namespace

TEST(HaFeatureBits, MatchHomeAssistantSource) {
    // homeassistant/components/media_player/const.py MediaPlayerEntityFeature
    namespace m = media_player_feature;
    EXPECT_EQ(m::kPause, 1u);
    EXPECT_EQ(m::kVolumeSet, 4u);
    EXPECT_EQ(m::kVolumeMute, 8u);
    EXPECT_EQ(m::kTurnOn, 128u);
    EXPECT_EQ(m::kTurnOff, 256u);
    EXPECT_EQ(m::kVolumeStep, 1024u);
    EXPECT_EQ(m::kSelectSource, 2048u);
    EXPECT_EQ(m::kPlay, 16384u);
    EXPECT_EQ(m::kSelectSoundMode, 65536u);
    EXPECT_EQ(m::kRepeatSet, 262144u);
    EXPECT_EQ(m::kSearchMedia, 4194304u);
    // homeassistant/components/light/const.py LightEntityFeature
    EXPECT_EQ(light_feature::kEffect, 4u);
    EXPECT_EQ(light_feature::kFlash, 8u);
    EXPECT_EQ(light_feature::kTransition, 32u);
    // homeassistant/components/fan/const.py FanEntityFeature
    EXPECT_EQ(fan_feature::kSetSpeed, 1u);
    EXPECT_EQ(fan_feature::kOscillate, 2u);
    EXPECT_EQ(fan_feature::kDirection, 4u);
    EXPECT_EQ(fan_feature::kPresetMode, 8u);
    EXPECT_EQ(fan_feature::kTurnOff, 16u);
    EXPECT_EQ(fan_feature::kTurnOn, 32u);
    // homeassistant/components/cover/const.py CoverEntityFeature
    EXPECT_EQ(cover_feature::kOpen, 1u);
    EXPECT_EQ(cover_feature::kClose, 2u);
    EXPECT_EQ(cover_feature::kSetPosition, 4u);
    EXPECT_EQ(cover_feature::kStop, 8u);
    EXPECT_EQ(cover_feature::kOpenTilt, 16u);
    EXPECT_EQ(cover_feature::kCloseTilt, 32u);
    EXPECT_EQ(cover_feature::kStopTilt, 64u);
    EXPECT_EQ(cover_feature::kSetTiltPosition, 128u);
    // homeassistant/components/humidifier/const.py HumidifierEntityFeature
    EXPECT_EQ(humidifier_feature::kModes, 1u);
}

TEST(HaCapabilities, NewerDomains) {
    using V = std::vector<std::string>;
    EXPECT_EQ(ids(EntityCapabilities::fromEntity(plain("scene.movie_night"))), V{"scene.turn_on"});
    EXPECT_EQ(ids(EntityCapabilities::fromEntity(plain("script.goodnight"))), (V{"script.turn_on", "script.turn_off"}));
    EXPECT_EQ(ids(EntityCapabilities::fromEntity(plain("button.restart_router"))), V{"button.press"});
    EXPECT_EQ(ids(EntityCapabilities::fromEntity(plain("input_boolean.guest_mode"))),
              (V{"input_boolean.turn_on", "input_boolean.turn_off", "input_boolean.toggle"}));
    EXPECT_EQ(ids(EntityCapabilities::fromEntity(blinds())),
              (V{"cover.open_cover", "cover.close_cover", "cover.stop_cover", "cover.toggle", "cover.set_cover_position"}));
    EntityInfo tilt = blinds();
    tilt.supportedFeatures = 1 | 16 | 32 | 64 | 128;  // OPEN alone: no toggle
    EXPECT_EQ(ids(EntityCapabilities::fromEntity(tilt)),
              (V{"cover.open_cover", "cover.open_cover_tilt", "cover.close_cover_tilt", "cover.stop_cover_tilt",
                 "cover.set_cover_tilt_position"}));
    EXPECT_EQ(ids(EntityCapabilities::fromEntity(humidifierEntity())),
              (V{"humidifier.turn_on", "humidifier.turn_off", "humidifier.toggle", "humidifier.set_humidity",
                 "humidifier.set_mode"}));
    EXPECT_EQ(ids(EntityCapabilities::fromEntity(dryerCycle())),
              (V{"select.select_option", "select.select_next", "select.select_previous"}));
    EXPECT_TRUE(EntityCapabilities::fromEntity(plain("select.empty")).empty()) << "no options: nothing to choose";
}

TEST(HaCapabilities, NewerDomainParams) {
    const auto h = EntityCapabilities::fromEntity(plain("humidifier.bare"));
    EXPECT_EQ(command(h, "humidifier.set_humidity").params[0].minInt, 0);
    EXPECT_EQ(command(h, "humidifier.set_humidity").params[0].maxInt, 100);
    const auto hum = EntityCapabilities::fromEntity(humidifierEntity());
    EXPECT_EQ(command(hum, "humidifier.set_humidity").params[0].minInt, 30);
    EXPECT_EQ(command(hum, "humidifier.set_humidity").params[0].maxInt, 80);
    EXPECT_STREQ(command(hum, "humidifier.set_humidity").params[0].unit, "%");

    const auto cover = EntityCapabilities::fromEntity(blinds());
    const ParamSpec &pos = command(cover, "cover.set_cover_position").params[0];
    EXPECT_STREQ(pos.name, "position");
    EXPECT_EQ(pos.type, ParamType::Int);
    EXPECT_EQ(pos.minInt, 0);
    EXPECT_EQ(pos.maxInt, 100);
    EXPECT_STREQ(pos.unit, "%");
}

TEST(HaServiceCall, NewerDomainsCallTheRightService) {
    ServiceCall c;
    const auto cover = EntityCapabilities::fromEntity(blinds());
    ASSERT_EQ(call(cover, "cover.set_cover_position", {in("position", ParamLiteral::ofInt(40))}, &c), ErrorCode::Ok);
    EXPECT_EQ(c.service, "set_cover_position");
    EXPECT_EQ(c.data[0].value.integer, 40);

    const auto select = EntityCapabilities::fromEntity(dryerCycle());
    ASSERT_EQ(call(select, "select.select_option", {in("option", ParamLiteral::ofString("towels"))}, &c),
              ErrorCode::Ok);
    EXPECT_EQ(c.service, "select_option");
    EXPECT_EQ(c.data[0].value.string, "towels");
    EXPECT_EQ(call(select, "select.select_option", {in("option", ParamLiteral::ofString("wool"))}, &c),
              ErrorCode::InvalidEnumValue);
}

TEST(HaSuggestedKind, ByDomainPlatformAndDeviceClass) {
    for (const char *deviceClass : {"shade", "blind", "garage", "gate", "door", ""}) {
        EXPECT_STREQ(suggestedKind("cover", "", deviceClass), "cover") << deviceClass;
    }
    EXPECT_STREQ(suggestedKind("scene", "", ""), "scene");
    EXPECT_STREQ(suggestedKind("script", "", ""), "script");
    EXPECT_STREQ(suggestedKind("humidifier", "", "humidifier"), "climate");
    EXPECT_STREQ(suggestedKind("fan", "", ""), "fan");
    EXPECT_STREQ(suggestedKind("light", "hue", ""), "light");
    EXPECT_STREQ(suggestedKind("switch", "", "outlet"), "switch");
    for (const char *d : {"button", "input_boolean", "select", "remote", "climate", "lock", "sensor", ""}) {
        EXPECT_STREQ(suggestedKind(d, "", ""), "other") << d;
    }
    EXPECT_STREQ(suggestedKind("media_player", "denonavr", "receiver"), "receiver");
    EXPECT_STREQ(suggestedKind("media_player", "sonos", "speaker"), "speaker");
    EXPECT_STREQ(suggestedKind("media_player", "alexa_media", ""), "speaker");
    EXPECT_STREQ(suggestedKind("media_player", "group", ""), "speaker");
    EXPECT_STREQ(suggestedKind("media_player", "apple_tv", ""), "streamer");
    EXPECT_STREQ(suggestedKind("media_player", "webostv", "tv"), "tv");
    EXPECT_STREQ(suggestedKind("media_player", "", ""), "tv");
}

TEST(HaCapabilities, TypicalFans) {
    const auto speed = EntityCapabilities::fromEntity(speedFan());
    EXPECT_EQ(ids(speed), fans({"turn_on", "turn_off", "set_percentage", "increase_speed", "decrease_speed"}));
    const CommandSpec &on = command(speed, "fan.turn_on");
    ASSERT_EQ(on.paramCount, 1) << "no preset_modes, no preset_mode";
    EXPECT_STREQ(on.params[0].name, "percentage");
    EXPECT_FALSE(on.params[0].required);
    const CommandSpec &set = command(speed, "fan.set_percentage");
    ASSERT_EQ(set.paramCount, 1);
    EXPECT_EQ(set.params[0].type, ParamType::Int);
    EXPECT_TRUE(set.params[0].required);
    EXPECT_EQ(set.params[0].minInt, 0);
    EXPECT_EQ(set.params[0].maxInt, 100);
    EXPECT_STREQ(set.params[0].unit, "%");
    EXPECT_EQ(set.params[0].label, nullptr) << "curated params stay unlabelled";
    EXPECT_EQ(set.params[0].step, 33.0f) << "100/3 rounded down: 99 is still the top speed";
    const CommandSpec &up = command(speed, "fan.increase_speed");
    ASSERT_EQ(up.paramCount, 1);
    EXPECT_STREQ(up.params[0].name, "percentage_step");
    EXPECT_FALSE(up.params[0].required);
    EXPECT_STREQ(up.params[0].unit, "%");

    const auto purifier = EntityCapabilities::fromEntity(presetFan());
    EXPECT_EQ(ids(purifier), fans({"turn_on", "turn_off", "set_percentage", "increase_speed", "decrease_speed",
                                   "set_preset_mode"}));
    const CommandSpec &purifierOn = command(purifier, "fan.turn_on");
    ASSERT_EQ(purifierOn.paramCount, 2);
    EXPECT_STREQ(purifierOn.params[1].name, "preset_mode");
    EXPECT_FALSE(purifierOn.params[1].required);
    const CommandSpec &preset = command(purifier, "fan.set_preset_mode");
    ASSERT_EQ(preset.paramCount, 1);
    EXPECT_TRUE(preset.params[0].required);
    ASSERT_EQ(preset.params[0].optionCount, 1);
    EXPECT_STREQ(preset.params[0].options[0], "sleep");

    const auto turning = EntityCapabilities::fromEntity(directionFan());
    EXPECT_EQ(ids(turning), fans({"turn_on", "turn_off", "set_percentage", "increase_speed", "decrease_speed",
                                 "set_direction", "set_preset_mode"}));
    EXPECT_EQ(command(turning, "fan.set_percentage").params[0].step, 14.0f);
    const CommandSpec &direction = command(turning, "fan.set_direction");
    ASSERT_EQ(direction.params[0].optionCount, 2);
    EXPECT_STREQ(direction.params[0].options[0], "forward");
    EXPECT_STREQ(direction.params[0].options[1], "reverse");

    ServiceCall c;
    ASSERT_EQ(call(turning, "fan.turn_on", {in("preset_mode", ParamLiteral::ofString("auto"))}, &c), ErrorCode::Ok);
    EXPECT_EQ(c.domain, "fan");
    EXPECT_EQ(c.service, "turn_on");
    EXPECT_EQ(c.entityId, "fan.box_fan");
    ASSERT_EQ(c.data.size(), 1u) << "the omitted percentage isn't sent";
    EXPECT_EQ(c.data[0].key, "preset_mode");
    EXPECT_EQ(c.data[0].value.string, "auto");
    ASSERT_EQ(call(turning, "fan.set_percentage", {in("percentage", ParamLiteral::ofInt(101))}, &c),
              ErrorCode::ParamOutOfRange);
}

TEST(HaCapabilities, FanGating) {
    EntityInfo e;
    e.entityId = "fan.x";
    e.supportedFeatures = fan_feature::kOscillate;
    const auto osc = EntityCapabilities::fromEntity(e);
    EXPECT_EQ(ids(osc), fans({"oscillate"})) << "no TURN_ON/TURN_OFF bits, no power";
    EXPECT_STREQ(command(osc, "fan.oscillate").params[0].name, "oscillating");
    EXPECT_TRUE(command(osc, "fan.oscillate").params[0].required);
    e.supportedFeatures = fan_feature::kPresetMode;
    EXPECT_TRUE(EntityCapabilities::fromEntity(e).empty()) << "no preset_modes, no preset picker";
    e.presetModes = {"auto"};
    EXPECT_EQ(ids(EntityCapabilities::fromEntity(e)), fans({"set_preset_mode"}));
    e.supportedFeatures = fan_feature::kSetSpeed;
    e.percentageStep = 0.5;  // 200 speeds: too fine for an int step
    const auto fine = EntityCapabilities::fromEntity(e);
    EXPECT_EQ(command(fine, "fan.set_percentage").params[0].step, 0.0f);
    EXPECT_GE(fine.catalog().find("fan.set_preset_mode"), 0) << "HA registers it for SET_SPEED too";
    e.supportedFeatures = 0xFFFFFFFF;
    for (const auto &id : ids(EntityCapabilities::fromEntity(e))) EXPECT_NE(id, "fan.toggle");
}

TEST(HaCapabilities, RealEntitiesMatchFixtures) {
    EXPECT_EQ(ids(EntityCapabilities::fromEntity(denon())),
              mp({"turn_on", "turn_off", "volume_up", "volume_down", "volume_set", "volume_mute",
                  "select_source", "select_sound_mode"}));
    EXPECT_EQ(ids(EntityCapabilities::fromEntity(sonos())),
              mp({"volume_up", "volume_down", "volume_set", "volume_mute", "media_play_pause",
                  "media_play", "media_pause", "media_stop", "media_next_track",
                  "media_previous_track", "select_source", "shuffle_set", "repeat_set"}));
    EXPECT_EQ(ids(EntityCapabilities::fromEntity(lgTv())),
              mp({"turn_on", "turn_off", "volume_up", "volume_down", "volume_mute",
                  "media_play_pause", "media_play", "media_pause", "media_stop",
                  "media_next_track", "media_previous_track", "select_source"}));
    EXPECT_EQ(ids(EntityCapabilities::fromEntity(xbox())),
              mp({"turn_on", "turn_off", "volume_up", "volume_down", "volume_mute",
                  "media_play_pause", "media_play", "media_pause", "media_next_track",
                  "media_previous_track"}));
    const auto light = EntityCapabilities::fromEntity(hue());
    EXPECT_EQ(ids(light), (std::vector<std::string>{"light.turn_on", "light.turn_off",
                                                    "light.brightness_step"}));
    const CommandSpec &on = command(light, "light.turn_on");
    ASSERT_EQ(on.paramCount, 3);
    EXPECT_STREQ(on.params[0].name, "brightness");
    EXPECT_EQ(on.params[0].maxInt, 255);
    EXPECT_STREQ(on.params[1].name, "color_temp_kelvin");
    EXPECT_EQ(on.params[1].minInt, 2000);
    EXPECT_EQ(on.params[1].maxInt, 6535);
    EXPECT_STREQ(on.params[2].name, "transition");
    for (uint8_t i = 0; i < on.paramCount; i++) EXPECT_FALSE(on.params[i].required);
}

TEST(HaCapabilities, FeatureGating) {
    EntityInfo e;
    e.entityId = "media_player.x";
    e.supportedFeatures = media_player_feature::kVolumeStep;
    EXPECT_EQ(ids(EntityCapabilities::fromEntity(e)), mp({"volume_up", "volume_down"}))
        << "VOLUME_STEP alone: no volume_set";
    e.supportedFeatures = media_player_feature::kPlay;
    EXPECT_EQ(ids(EntityCapabilities::fromEntity(e)), mp({"media_play"}))
        << "play_pause needs PLAY and PAUSE";
    e.supportedFeatures = media_player_feature::kSelectSource;
    EXPECT_TRUE(EntityCapabilities::fromEntity(e).empty()) << "no source_list, no select_source";
    // Never offered whatever the flags say.
    e.supportedFeatures = 0xFFFFFFFF;
    e.sourceList = {"A"};
    e.soundModeList = {"B"};
    for (const auto &id : ids(EntityCapabilities::fromEntity(e))) {
        EXPECT_EQ(id.find("play_media"), std::string::npos);
        EXPECT_EQ(id.find("join"), std::string::npos);
        EXPECT_EQ(id.find("toggle"), std::string::npos);
        EXPECT_EQ(id.find("seek"), std::string::npos);
        EXPECT_EQ(id.find("clear_playlist"), std::string::npos);
        EXPECT_EQ(id.find("announce"), std::string::npos);
    }
}

TEST(HaCapabilities, LightModes) {
    EntityInfo onoff;
    onoff.entityId = "light.porch";
    onoff.supportedColorModes = {"onoff"};
    const auto plain = EntityCapabilities::fromEntity(onoff);
    EXPECT_EQ(ids(plain), (std::vector<std::string>{"light.turn_on", "light.turn_off"}));
    EXPECT_EQ(command(plain, "light.turn_on").paramCount, 0);

    EntityInfo dimmable = onoff;
    dimmable.supportedColorModes = {"brightness"};
    const auto dim = EntityCapabilities::fromEntity(dimmable);
    EXPECT_EQ(command(dim, "light.turn_on").paramCount, 1) << "no transition flag, no colour temp";

    EntityInfo badRange = hue();
    badRange.minColorTempKelvin = 7000;
    badRange.maxColorTempKelvin = 2000;
    const auto fixed = EntityCapabilities::fromEntity(badRange);
    EXPECT_EQ(command(fixed, "light.turn_on").params[1].minInt, kDefaultMinKelvin);
}

TEST(HaCapabilities, UnsupportedDomainsAndBadIdsGetNothing) {
    for (const char *id : {"remote.living_room_hub", "climate.hallway", "lock.front_door", "vacuum.kitchen",
                           "alarm_control_panel.home", "valve.main", "scene.all", "group.all_lights", "homeassistant.restart",
                           "light.all", "light.none", "all", "light.a,light.b", "Light.Kitchen",
                           "light.", ".kitchen", "light.kit chen", "light.kitchen\n", ""}) {
        EntityInfo e;
        e.entityId = id;
        e.supportedFeatures = 0xFFFFFFFF;
        e.supportedColorModes = {"brightness"};
        EXPECT_TRUE(EntityCapabilities::fromEntity(e).empty()) << id;
    }
    EXPECT_TRUE(isValidEntityId("media_player.family_room_denon"));
    EXPECT_FALSE(isValidEntityId("light.all"));
    EXPECT_TRUE(isValidEntityId(std::string("light.") + std::string(limits::kMaxProviderRefLength - 6, 'a')));
    EXPECT_FALSE(isValidEntityId(std::string("light.") + std::string(limits::kMaxProviderRefLength - 5, 'a')));
}

TEST(HaCapabilities, OptionLimits) {
    EntityInfo e = lgTv();
    e.sourceList.push_back(std::string(limits::kMaxParamStringLength + 1, 'x'));  // longer than kMaxParamStringLength
    e.sourceList.push_back("HDMI 1");              // duplicate
    e.sourceList.push_back("");
    const auto caps = EntityCapabilities::fromEntity(e);
    EXPECT_EQ(command(caps, "media_player.select_source").params[0].optionCount, 6);
    EXPECT_EQ(caps.droppedOptions(), 3u);

    e.sourceList = {std::string(limits::kMaxParamStringLength + 1, 'y')};
    const auto none = EntityCapabilities::fromEntity(e);
    EXPECT_LT(none.catalog().find("media_player.select_source"), 0)
        << "a source picker with no usable options is dropped";
}

TEST(HaCapabilities, CatalogSurvivesMove) {
    auto original = EntityCapabilities::fromEntity(lgTv());
    std::vector<EntityCapabilities> store;
    store.push_back(std::move(original));
    store.push_back(EntityCapabilities::fromEntity(denon()));
    store.push_back(EntityCapabilities::fromEntity(hue()));  // forces a reallocation
    const CommandSpec &select = command(store[0], "media_player.select_source");
    EXPECT_STREQ(select.params[0].options[3], "Live TV");
}

TEST(HaServiceCall, VolumeSetAndSelectSource) {
    const auto caps = EntityCapabilities::fromEntity(denon());
    ServiceCall c;
    ASSERT_EQ(call(caps, "media_player.volume_set", {in("level", ParamLiteral::ofNumber(0.35))}, &c),
              ErrorCode::Ok);
    EXPECT_EQ(c.domain, "media_player");
    EXPECT_EQ(c.service, "volume_set");
    EXPECT_EQ(c.entityId, "media_player.family_room_denon");
    ASSERT_EQ(c.data.size(), 1u);
    EXPECT_EQ(c.data[0].key, "volume_level");
    EXPECT_NEAR(c.data[0].value.number, 0.35, 1e-6);

    ASSERT_EQ(call(caps, "media_player.select_source", {in("source", ParamLiteral::ofString("Game"))}, &c),
              ErrorCode::Ok);
    ASSERT_EQ(c.data.size(), 1u);
    EXPECT_EQ(c.data[0].key, "source");
    EXPECT_EQ(c.data[0].value.string, "Game");

    ASSERT_EQ(call(caps, "media_player.volume_up", {}, &c), ErrorCode::Ok);
    EXPECT_TRUE(c.data.empty());

    ASSERT_EQ(call(caps, "media_player.volume_mute", {in("muted", ParamLiteral::ofBool(true))}, &c),
              ErrorCode::Ok);
    EXPECT_EQ(c.data[0].key, "is_volume_muted");
    EXPECT_TRUE(c.data[0].value.boolean);
}

TEST(HaServiceCall, OmittedOptionalParamsAreNotSent) {
    const auto caps = EntityCapabilities::fromEntity(hue());
    ServiceCall c;
    ASSERT_EQ(call(caps, "light.turn_on", {}, &c), ErrorCode::Ok);
    EXPECT_EQ(c.service, "turn_on");
    EXPECT_TRUE(c.data.empty()) << "turn_on with no params must not invent a brightness";

    ASSERT_EQ(call(caps, "light.turn_on", {in("brightness", ParamLiteral::ofInt(0)),
                                          in("transition", ParamLiteral::ofNumber(1.5))}, &c),
              ErrorCode::Ok);
    ASSERT_EQ(c.data.size(), 2u);
    EXPECT_EQ(c.data[0].key, "brightness");
    EXPECT_EQ(c.data[0].value.integer, 0);
    EXPECT_EQ(c.data[1].key, "transition");

    ASSERT_EQ(call(caps, "light.brightness_step", {in("step", ParamLiteral::ofInt(-32))}, &c),
              ErrorCode::Ok);
    EXPECT_EQ(c.service, "turn_on");
    ASSERT_EQ(c.data.size(), 1u);
    EXPECT_EQ(c.data[0].key, "brightness_step");
    EXPECT_EQ(c.data[0].value.integer, -32);
}

// Malicious and malformed inputs. The JSON layer only produces scalars
// (ParamLiteral has no object/array kind, and the OpenAPI schema refuses
// nested values), so what's left is names and values.
TEST(HaServiceCall, RejectsTargetsRangesAndTypes) {
    const auto denonCaps = EntityCapabilities::fromEntity(denon());
    const auto light = EntityCapabilities::fromEntity(hue());
    ServiceCall c;
    std::string bad;

    for (const char *name : {"entity_id", "area_id", "device_id", "target", "service_data",
                             "data", "service", "floor_id", "label_id", "domain"}) {
        EXPECT_TRUE(isForbiddenParamName(name)) << name;
        EXPECT_EQ(call(light, "light.turn_on", {in(name, ParamLiteral::ofString("all"))}, &c, &bad),
                  ErrorCode::UnknownParam)
            << name;
        EXPECT_EQ(bad, name);
    }
    EXPECT_FALSE(isForbiddenParamName("brightness"));

    // Extra param smuggled next to a valid one.
    EXPECT_EQ(call(light, "light.turn_on", {in("brightness", ParamLiteral::ofInt(10)),
                                            in("entity_id", ParamLiteral::ofString("light.all"))}, &c),
              ErrorCode::UnknownParam);
    // Duplicate key (a JSON object with "brightness" twice).
    EXPECT_EQ(call(light, "light.turn_on", {in("brightness", ParamLiteral::ofInt(10)),
                                            in("brightness", ParamLiteral::ofInt(255))}, &c),
              ErrorCode::DuplicateParam);
    // Ranges.
    EXPECT_EQ(call(denonCaps, "media_player.volume_set", {in("level", ParamLiteral::ofNumber(1.5))}, &c),
              ErrorCode::ParamOutOfRange);
    EXPECT_EQ(call(denonCaps, "media_player.volume_set", {in("level", ParamLiteral::ofNumber(-0.1))}, &c),
              ErrorCode::ParamOutOfRange);
    EXPECT_EQ(call(denonCaps, "media_player.volume_set",
                   {in("level", ParamLiteral::ofNumber(std::numeric_limits<double>::quiet_NaN()))}, &c),
              ErrorCode::ParamOutOfRange);
    EXPECT_EQ(call(light, "light.turn_on", {in("brightness", ParamLiteral::ofInt(256))}, &c),
              ErrorCode::ParamOutOfRange);
    EXPECT_EQ(call(light, "light.turn_on", {in("color_temp_kelvin", ParamLiteral::ofInt(10000))}, &c),
              ErrorCode::ParamOutOfRange);
    // Types.
    EXPECT_EQ(call(light, "light.turn_on", {in("brightness", ParamLiteral::ofString("255"))}, &c),
              ErrorCode::InvalidParamType);
    EXPECT_EQ(call(denonCaps, "media_player.volume_mute", {in("muted", ParamLiteral::ofInt(1))}, &c),
              ErrorCode::InvalidParamType);
    // Enum values outside the entity's own list.
    EXPECT_EQ(call(denonCaps, "media_player.select_source",
                   {in("source", ParamLiteral::ofString("game"))}, &c),
              ErrorCode::InvalidEnumValue);
    EXPECT_EQ(call(denonCaps, "media_player.select_source", {}, &c), ErrorCode::MissingParam);
    // Command this entity doesn't have.
    EXPECT_EQ(call(denonCaps, "media_player.play_media", {}, &c), ErrorCode::UnknownCommand);
    EXPECT_EQ(call(denonCaps, "homeassistant.turn_off", {}, &c), ErrorCode::UnknownCommand);
}

TEST(HaServiceCall, RejectsValuesNotFromThisCatalog) {
    const auto caps = EntityCapabilities::fromEntity(denon());
    ServiceCall c;
    const int select = caps.catalog().find("media_player.select_source");
    ParamValue outOfRange = ParamValue::ofOption(200);
    EXPECT_EQ(caps.buildServiceCall(static_cast<uint16_t>(select), &outOfRange, 1, &c),
              ErrorCode::InvalidEnumValue);
    ParamValue wrongType = ParamValue::ofInt(1);
    EXPECT_EQ(caps.buildServiceCall(static_cast<uint16_t>(select), &wrongType, 1, &c),
              ErrorCode::InvalidParamType);
    EXPECT_EQ(caps.buildServiceCall(999, nullptr, 0, &c), ErrorCode::UnknownCommand);
}

TEST(HaServiceCall, OnlyAllowlistedKeysEverReachHomeAssistant) {
    const char *allowed[] = {"volume_level", "is_volume_muted", "source", "sound_mode", "shuffle",
                             "repeat", "brightness", "color_temp_kelvin", "transition",
                             "brightness_step", "percentage", "preset_mode", "percentage_step",
                             "oscillating", "direction", "position", "tilt_position", "humidity", "mode", "option"};
    EntityInfo all;
    all.entityId = "media_player.everything";
    all.supportedFeatures = 0xFFFFFFFF;
    all.sourceList = {"A"};
    all.soundModeList = {"B"};
    EntityInfo fan = directionFan();
    fan.supportedFeatures = 0xFFFFFFFF;
    std::vector<EntityInfo> entities{all, hue(), sonos(), fan, blinds(), humidifierEntity(), dryerCycle(), plain("scene.movie_night"),
                                     plain("script.goodnight"), plain("button.restart_router"),
                                     plain("input_boolean.guest_mode")};
    for (EntityInfo &e : entities) {
        if (e.entityId.rfind("media_player.", 0) != 0) e.supportedFeatures = 0xFFFFFFFF;
    }
    for (const EntityInfo &e : entities) {
        const auto caps = EntityCapabilities::fromEntity(e);
        const CommandCatalog c = caps.catalog();
        for (uint16_t i = 0; i < c.count; i++) {
            ParamValue values[limits::kMaxParams];
            std::string unused;
            std::vector<ParamInput> inputs;
            for (uint8_t p = 0; p < c.items[i].paramCount; p++) {
                const ParamSpec &spec = c.items[i].params[p];
                ParamLiteral v;
                switch (spec.type) {
                    case ParamType::Bool: v = ParamLiteral::ofBool(true); break;
                    case ParamType::Int: v = ParamLiteral::ofInt(spec.minInt); break;
                    case ParamType::Number: v = ParamLiteral::ofNumber(spec.minNumber); break;
                    case ParamType::Enum: v = ParamLiteral::ofString(spec.options[0]); break;
                    case ParamType::ActivityRef: FAIL();
                }
                EXPECT_FALSE(isForbiddenParamName(spec.name));
                inputs.push_back({spec.name, v});
            }
            uint8_t present = 0;
            ASSERT_EQ(resolveParams(c.items[i], inputs, values, &unused, nullptr, &present), ErrorCode::Ok);
            ServiceCall sc;
            ASSERT_EQ(caps.buildServiceCall(i, values, present, &sc), ErrorCode::Ok) << c.items[i].id;
            EXPECT_EQ(sc.data.size(), c.items[i].paramCount) << "every supplied param is sent";
            EXPECT_EQ(sc.entityId, e.entityId);
            EXPECT_EQ(sc.domain, e.entityId.substr(0, e.entityId.find('.')));
            for (const auto &field : sc.data) {
                bool ok = false;
                for (const char *a : allowed) ok = ok || field.key == a;
                EXPECT_TRUE(ok) << field.key;
            }
            EXPECT_NE(commandLabel(c.items[i].id), nullptr) << c.items[i].id;
        }
    }
}
