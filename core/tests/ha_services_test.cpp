// Integration-specific HA services (ha/services.h): selector normalization,
// the stored snapshot and the commands built from it.
#include <gtest/gtest.h>

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "hubbo/domain/limits.h"
#include "hubbo/domain/params.h"
#include "hubbo/ha/capabilities.h"
#include "hubbo/ha/services.h"
#include "hubbo/ha/snapshot.h"
#include "hubbo/ha/snapshot_json.h"

using namespace hubbo;
using namespace hubbo::ha;

namespace {

// Shaped like HA's get_services result (homeassistant/components/sonos,
// denonavr services.yaml after validation: feature names are ints).
const char *kServices = R"({
  "media_player": {
    "play_media": {"name": "Play media", "target": {"entity": [{"domain": ["media_player"]}]},
                   "fields": {"media_content_id": {"required": true, "selector": {"text": {}}}}}
  },
  "sonos": {
    "set_sleep_timer": {
      "name": "Set timer",
      "target": {"entity": [{"integration": "sonos", "domain": ["media_player"]}]},
      "fields": {"sleep_time": {"name": "Sleep time", "selector":
                 {"number": {"min": 0, "max": 86399, "unit_of_measurement": "seconds"}}}}
    },
    "clear_sleep_timer": {
      "name": "Clear timer",
      "target": {"entity": {"integration": "sonos", "domain": "media_player"}}
    },
    "update_alarm": {
      "target": {"entity": [{"integration": "sonos", "domain": ["media_player"]}]},
      "fields": {"alarm_id": {"required": true, "selector": {"number": {"min": 1, "max": 1440}}},
                 "time": {"selector": {"time": {}}}}
    },
    "snapshot": {
      "fields": {"entity_id": {"selector": {"entity": {"integration": "sonos"}}}}
    },
    "set_eq": {
      "name": "Set EQ",
      "target": {"entity": [{"integration": "sonos", "domain": ["media_player"]}]},
      "fields": {
        "night_mode": {"selector": {"boolean": {}}},
        "advanced": {"collapsed": true, "fields": {
          "bass": {"selector": {"number": {"min": -10, "max": 10, "step": 1}}},
          "balance": {"selector": {"number": {"min": -1, "max": 1, "step": 0.1}}},
          "preset": {"selector": {"select": {"options": ["flat", {"value": "voice", "label": "Voice"}]}}}
        }}
      }
    },
    "too_many": {
      "target": {"entity": [{"integration": "sonos"}]},
      "fields": {"a": {"selector": {"boolean": {}}}, "b": {"selector": {"boolean": {}}},
                 "c": {"selector": {"boolean": {}}}, "d": {"selector": {"boolean": {}}},
                 "e": {"selector": {"boolean": {}}}}
    },
    "sneaky": {
      "target": {"entity": [{"integration": "sonos"}]},
      "fields": {"entity_id": {"selector": {"select": {"options": ["media_player.all"]}}}}
    },
    "volume_feature": {
      "target": {"entity": [{"integration": "sonos", "supported_features": [4]}]},
      "fields": {"level": {"selector": {"number": {"min": 0, "max": 1, "step": "any"}}},
                 "mute": {"filter": {"supported_features": [8]}, "selector": {"boolean": {}}}}
    },
    "multi": {
      "target": {"entity": [{"integration": "sonos"}]},
      "fields": {"rooms": {"selector": {"select": {"multiple": true, "options": ["a", "b"]}}}}
    }
  },
  "denonavr": {
    "get_command": {
      "target": {"entity": [{"integration": "denonavr", "domain": ["media_player"]}]},
      "fields": {"command": {"required": true, "selector": {"text": {}}}}
    }
  }
})";

std::vector<std::string> ids(const std::vector<SnapshotService> &services) {
    std::vector<std::string> out;
    for (const auto &s : services) out.push_back(s.service);
    return out;
}

// "<service> <reason>" for each skipped service.
std::vector<std::string> skips(const ServiceDirectory::Offer &offer) {
    std::vector<std::string> out;
    for (const auto &s : offer.skipped) out.push_back(s.service + " " + ServiceDirectory::skipReasonName(s.reason));
    return out;
}

// encode -> text -> decode: the decode must pass and give back the same snapshot.
::testing::AssertionResult roundTrip(const CapabilitySnapshot &snapshot, CapabilitySnapshot *decoded) {
    JsonDocument doc;
    encodeSnapshot(snapshot, doc.to<JsonObject>());
    std::string text;
    serializeJson(doc, text);
    JsonDocument back;
    if (deserializeJson(back, text)) return ::testing::AssertionFailure() << "unparseable: " << text;
    ValidationReport report;
    if (!decodeSnapshot(back.as<JsonVariantConst>(), decoded, &report, "/ha")) {
        return ::testing::AssertionFailure() << (report.issues().empty() ? "" : report.issues()[0].path) << " in " << text;
    }
    if (!(*decoded == snapshot)) return ::testing::AssertionFailure() << "changed: " << text;
    return ::testing::AssertionSuccess();
}

}  // namespace

TEST(HaServices, OffersTargetedTypedServicesOfTheEntitysIntegration) {
    const ServiceDirectory dir = ServiceDirectory::parse(kServices, {"sonos", "denonavr"});
    const auto offer = dir.offerFor("media_player.kitchen", "sonos", /*features=*/4);
    EXPECT_EQ(ids(offer.services),
              (std::vector<std::string>{"sonos.clear_sleep_timer", "sonos.set_eq", "sonos.set_sleep_timer",
                                        "sonos.volume_feature"}));
    // Matching the entity but not offerable: a time selector, five fields,
    // a target-like field name, a multiple select. `snapshot` has no entity
    // target at all, so it's neither offered nor counted.
    EXPECT_EQ(skips(offer), (std::vector<std::string>{"sonos.multi unsupported_field", "sonos.sneaky unsupported_field",
                                                      "sonos.too_many too_many_fields",
                                                      "sonos.update_alarm unsupported_field"}));

    const SnapshotService &timer = offer.services[2];
    ASSERT_EQ(timer.fields.size(), 1u);
    EXPECT_EQ(timer.label, "Set timer");
    EXPECT_EQ(timer.fields[0].name, "sleep_time");
    EXPECT_EQ(timer.fields[0].type, SnapshotParamType::Int);
    EXPECT_EQ(timer.fields[0].maximum, 86399);
    EXPECT_EQ(timer.fields[0].unit, "s") << "HA's \"seconds\", shortened";

    // A collapsed section is flattened; number -> int or number; select -> enum.
    const SnapshotService &eq = offer.services[1];
    ASSERT_EQ(eq.fields.size(), 4u);
    EXPECT_EQ(eq.fields[0].type, SnapshotParamType::Bool);
    EXPECT_EQ(eq.fields[1].type, SnapshotParamType::Int);
    EXPECT_EQ(eq.fields[2].type, SnapshotParamType::Number);
    EXPECT_EQ(eq.fields[3].type, SnapshotParamType::Enum);
    EXPECT_EQ(eq.fields[3].options, (std::vector<std::string>{"flat", "voice"}));

    // A field whose own feature filter the entity doesn't meet is left out.
    const SnapshotService &volume = offer.services[3];
    ASSERT_EQ(volume.fields.size(), 1u);
    EXPECT_EQ(volume.fields[0].name, "level");
    EXPECT_EQ(volume.fields[0].type, SnapshotParamType::Number);
    EXPECT_EQ(dir.offerFor("media_player.kitchen", "sonos", 4 | 8).services[3].fields.size(), 2u);
}

TEST(HaServices, TargetFilterDecidesWhoGetsAService) {
    const ServiceDirectory dir = ServiceDirectory::parse(kServices, {"sonos", "denonavr"});
    // Without the feature its target requires, volume_feature isn't offered.
    EXPECT_EQ(ids(dir.offerFor("media_player.kitchen", "sonos", 0).services),
              (std::vector<std::string>{"sonos.clear_sleep_timer", "sonos.set_eq", "sonos.set_sleep_timer"}));
    // Another domain of the same integration: only domain-less filters match.
    const auto sw = dir.offerFor("switch.kitchen_crossfade", "sonos", 0);
    for (const auto &s : sw.services) EXPECT_NE(s.service, "sonos.set_sleep_timer");
    // Another integration's entity gets none of sonos' services; denonavr's
    // only service takes free text, which Hubbo can't type yet.
    const auto denon = dir.offerFor("media_player.denon", "denonavr", 0);
    EXPECT_TRUE(denon.services.empty());
    EXPECT_EQ(skips(denon), (std::vector<std::string>{"denonavr.get_command unsupported_field"}));
    // The domain's own services are the curated allowlist, never offered here.
    EXPECT_TRUE(dir.offerFor("media_player.kitchen", "media_player", 0xFFFFFFFF).services.empty());
    // No platform (not in the registry): nothing.
    EXPECT_TRUE(dir.offerFor("media_player.kitchen", "", 0).services.empty());
}

// Real HA (2026.11, sim/fixtures/real-ha-sample.json): get_services carries
// no names; they come from frontend/get_translations.
const char *kLifx = R"({
  "lifx": {
    "effect_flame": {
      "fields": {"speed": {"default": 3, "selector": {"number": {"min": 1.0, "max": 25.0, "step": 1.0,
                                                                "unit_of_measurement": "seconds", "mode": "slider"}}},
                 "power_on": {"default": true, "selector": {"boolean": {}}}},
      "target": {"entity": [{"integration": "lifx", "domain": ["light"]}]}
    },
    "set_hev_cycle_state": {
      "fields": {"power": {"required": true, "selector": {"boolean": {}}},
                 "duration": {"default": "big", "selector": {"number": {"min": 0, "max": 86400,
                                                                      "unit_of_measurement": "a unit far too long"}}},
                 "mode": {"default": "slow", "selector": {"select": {"options": ["fast", "slow"]}}},
                 "level": {"default": 2.5, "selector": {"number": {"min": 0, "max": 1, "step": "any"}}}},
      "target": {"entity": [{"integration": "lifx", "domain": ["light"]}]}
    },
    "get_queue": {"fields": {}, "target": {"entity": [{"integration": "lifx", "domain": ["light"]}]},
                  "response": {"optional": false}},
    "get_maybe": {"fields": {}, "target": {"entity": [{"integration": "lifx", "domain": ["light"]}]},
                  "response": {"optional": true}}
  }
})";
const char *kLifxNames = R"({
  "component.lifx.services.effect_flame.name": "Flame effect",
  "component.lifx.services.effect_flame.description": "Sets a flame effect.",
  "component.lifx.services.effect_flame.fields.power_on.name": "Power on",
  "component.lifx.services.effect_flame.fields.power_on.description": "Powered off lights will be turned on\n  before starting the effect.",
  "component.lifx.services.set_hev_cycle_state.fields.duration.description": "How long the HEV cycle runs, in seconds. A cycle of zero means the bulb's default duration is used, which is two hours unless it was changed in the LIFX app, so set it here to override that default.",
  "component.lifx.services.effect_flame.fields.speed.name": "Speed",
  "component.sonos.services.effect_flame.name": "Not this one"
})";

TEST(HaServices, NamesDefaultsUnitsAndResponseOnly) {
    const ServiceDirectory dir = ServiceDirectory::parse(kLifx, {"lifx"}, kLifxNames);
    const auto offer = dir.offerFor("light.desk", "lifx", 0);
    EXPECT_EQ(ids(offer.services),
              (std::vector<std::string>{"lifx.effect_flame", "lifx.get_maybe", "lifx.set_hev_cycle_state"}));
    EXPECT_EQ(skips(offer), (std::vector<std::string>{"lifx.get_queue response_only"}));

    const SnapshotService &flame = offer.services[0];
    EXPECT_EQ(flame.label, "Flame effect");
    ASSERT_EQ(flame.fields.size(), 2u);
    EXPECT_EQ(flame.fields[0].name, "speed");
    EXPECT_EQ(flame.fields[0].label, "Speed");
    EXPECT_EQ(flame.fields[0].unit, "s");
    EXPECT_TRUE(flame.fields[0].hasDefault);
    EXPECT_EQ(flame.fields[0].defaultNumber, 3);
    EXPECT_EQ(flame.fields[1].label, "Power on");
    EXPECT_EQ(flame.fields[1].description, "Powered off lights will be turned on before starting the effect.");
    EXPECT_EQ(flame.fields[0].description, "") << "absent when HA has none";
    EXPECT_TRUE(flame.fields[1].hasDefault);
    EXPECT_EQ(flame.fields[1].defaultNumber, 1);

    // No translation: the id made readable. A default that doesn't fit is left out.
    const SnapshotService &hev = offer.services[2];
    EXPECT_EQ(hev.label, "Set hev cycle state");
    ASSERT_EQ(hev.fields.size(), 4u);
    EXPECT_EQ(hev.fields[0].label, "Power");
    EXPECT_FALSE(hev.fields[0].hasDefault);
    EXPECT_FALSE(hev.fields[1].hasDefault) << "a string default on a number";
    EXPECT_EQ(hev.fields[1].unit, "") << "too long to show; never cut";
    const std::string &cut = hev.fields[1].description;
    EXPECT_LE(cut.size(), kMaxSnapshotDescriptionLength);
    EXPECT_EQ(cut.substr(cut.size() - 3), "\xE2\x80\xA6") << cut;
    EXPECT_NE(cut[cut.size() - 4], ' ') << "cut at a word, then the ellipsis";
    EXPECT_EQ(cut.rfind("How long the HEV cycle runs", 0), 0u);
    EXPECT_TRUE(hev.fields[2].hasDefault);
    EXPECT_EQ(hev.fields[2].defaultOption, "slow");
    EXPECT_FALSE(hev.fields[3].hasDefault) << "out of range";

    // Without translations (or with junk) the readable fallback still applies.
    for (const char *names : {"", "{not json", "[1]"}) {
        const auto bare = ServiceDirectory::parse(kLifx, {"lifx"}, names).offerFor("light.desk", "lifx", 0);
        ASSERT_FALSE(bare.services.empty());
        EXPECT_EQ(bare.services[0].label, "Effect flame") << names;
        EXPECT_EQ(bare.services[0].fields[1].label, "Power on") << names;
    }
}

TEST(HaSnapshot, DefaultsRoundTripAndReachTheCommand) {
    const ServiceDirectory dir = ServiceDirectory::parse(kLifx, {"lifx"}, kLifxNames);
    EntityInfo entity;
    entity.entityId = "light.desk";
    const CapabilitySnapshot snapshot = makeSnapshot(entity, "lifx", dir);
    CapabilitySnapshot decoded;
    ASSERT_TRUE(roundTrip(snapshot, &decoded));

    const EntityCapabilities caps = EntityCapabilities::fromSnapshot(entity.entityId, decoded);
    const CommandCatalog catalog = caps.catalog();
    const int flame = catalog.find("lifx.effect_flame");
    ASSERT_GE(flame, 0);
    const CommandSpec &spec = catalog.items[flame];
    EXPECT_STREQ(spec.params[0].label, "Speed");
    EXPECT_STREQ(spec.params[0].unit, "s");
    EXPECT_EQ(spec.params[0].description, nullptr);
    EXPECT_STREQ(spec.params[1].description, "Powered off lights will be turned on before starting the effect.");
    EXPECT_TRUE(spec.params[0].hasDefault);
    EXPECT_EQ(spec.params[0].defaultValue.integer, 3);
    EXPECT_TRUE(spec.params[1].hasDefault);
    EXPECT_TRUE(spec.params[1].defaultValue.asBool());
    const int hev = catalog.find("lifx.set_hev_cycle_state");
    ASSERT_GE(hev, 0);
    EXPECT_FALSE(catalog.items[hev].params[0].hasDefault);
    EXPECT_TRUE(catalog.items[hev].params[2].hasDefault);
    EXPECT_EQ(catalog.items[hev].params[2].defaultValue.asOption(), 1);
}

// An enum default is found by value after the options are filtered, so a
// dropped option ahead of it doesn't shift it (and a dropped default is gone).
TEST(HaSnapshot, EnumDefaultSurvivesOptionFiltering) {
    const std::string tooLong(limits::kMaxParamStringLength + 1, 'x');
    SnapshotParam mode;
    mode.name = "mode";
    mode.type = SnapshotParamType::Enum;
    mode.options = {tooLong, "a", "b"};
    mode.hasDefault = true;
    mode.defaultOption = "b";
    SnapshotParam gone = mode;
    gone.name = "gone";
    gone.defaultOption = tooLong;
    CapabilitySnapshot s;
    s.platform = "sonos";
    s.services = {{"sonos.x", "", {mode, gone}}};
    const EntityCapabilities caps = EntityCapabilities::fromSnapshot("media_player.kitchen", s);
    const CommandCatalog catalog = caps.catalog();
    const int x = catalog.find("sonos.x");
    ASSERT_GE(x, 0);
    const ParamSpec &kept = catalog.items[x].params[0];
    ASSERT_EQ(kept.optionCount, 2);
    EXPECT_TRUE(kept.hasDefault);
    EXPECT_STREQ(kept.options[kept.defaultValue.asOption()], "b");
    EXPECT_FALSE(catalog.items[x].params[1].hasDefault) << "its default was filtered out";
    EXPECT_EQ(catalog.items[x].params[1].defaultValue.asOption(), 0);
}

TEST(HaServices, OnlyRequestedPlatformsAreParsedAndJunkIsHarmless) {
    EXPECT_EQ(ServiceDirectory::parse(kServices, {"denonavr"}).size(), 1u);
    EXPECT_EQ(ServiceDirectory::parse(kServices, {}).size(), 0u);
    EXPECT_EQ(ServiceDirectory::parse("{not json", {"sonos"}).size(), 0u);
    EXPECT_EQ(ServiceDirectory::parse(R"({"sonos": [1, 2]})", {"sonos"}).size(), 0u);
    const ServiceDirectory odd = ServiceDirectory::parse(
        R"({"sonos": {"x": {"target": {"entity": [{"supported_features": ["media_player.Feature.X"]}]}}}})",
        {"sonos"});
    EXPECT_TRUE(odd.offerFor("media_player.a", "sonos", 0xFFFFFFFF).services.empty())
        << "a feature filter Hubbo can't read never matches";
}

TEST(HaSnapshot, RoundTripsAndBuildsCommands) {
    const ServiceDirectory dir = ServiceDirectory::parse(kServices, {"sonos"});
    EntityInfo entity;
    entity.entityId = "media_player.kitchen";
    entity.supportedFeatures = media_player_feature::kVolumeSet | media_player_feature::kSelectSource;
    entity.sourceList = {"TV", "Line-in"};
    const CapabilitySnapshot snapshot = makeSnapshot(entity, "sonos", dir);
    ASSERT_EQ(snapshot.services.size(), 4u);

    CapabilitySnapshot decoded;
    ASSERT_TRUE(roundTrip(snapshot, &decoded));

    // The built-in allowlist plus the services, from the snapshot alone.
    const EntityCapabilities caps = EntityCapabilities::fromSnapshot(entity.entityId, decoded);
    const CommandCatalog catalog = caps.catalog();
    EXPECT_GE(catalog.find("media_player.volume_set"), 0);
    EXPECT_GE(catalog.find("media_player.select_source"), 0);
    const int timer = catalog.find("sonos.set_sleep_timer");
    ASSERT_GE(timer, 0);
    // Its label travels in the snapshot (the API shows it from there).
    bool labelled = false;
    for (const SnapshotService &s : decoded.services) labelled = labelled || (s.service == "sonos.set_sleep_timer" && s.label == "Set timer");
    EXPECT_TRUE(labelled);
    const CommandSpec &spec = catalog.items[timer];
    ASSERT_EQ(spec.paramCount, 1);
    EXPECT_STREQ(spec.params[0].name, "sleep_time");
    EXPECT_EQ(spec.params[0].type, ParamType::Int);
    EXPECT_EQ(spec.params[0].maxInt, 86399);

    // The call targets the entity, with the field as the data key.
    ParamValue params[1] = {ParamValue::ofInt(600)};
    ServiceCall call;
    ASSERT_EQ(caps.buildServiceCall(static_cast<uint16_t>(timer), params, 1, &call), ErrorCode::Ok);
    EXPECT_EQ(call.domain, "sonos");
    EXPECT_EQ(call.service, "set_sleep_timer");
    EXPECT_EQ(call.entityId, "media_player.kitchen");
    ASSERT_EQ(call.data.size(), 1u);
    EXPECT_EQ(call.data[0].key, "sleep_time");
    EXPECT_EQ(call.data[0].value.integer, 600);

    // Enum options from the snapshot.
    const int eq = catalog.find("sonos.set_eq");
    ASSERT_GE(eq, 0);
    ParamValue eqParams[4] = {ParamValue::ofBool(true), ParamValue::ofInt(0), ParamValue::ofNumber(0),
                              ParamValue::ofOption(1)};
    ASSERT_EQ(caps.buildServiceCall(static_cast<uint16_t>(eq), eqParams, 0b1001, &call), ErrorCode::Ok);
    ASSERT_EQ(call.data.size(), 2u) << "optional fields not supplied are left out";
    EXPECT_EQ(call.data[1].key, "preset");
    EXPECT_EQ(call.data[1].value.string, "voice");
}

TEST(HaSnapshot, DecodeIsStrict) {
    auto decode = [](const std::string &json, std::string *firstIssue) {
        JsonDocument doc;
        EXPECT_FALSE(deserializeJson(doc, json)) << json;
        CapabilitySnapshot s;
        ValidationReport report;
        const bool ok = decodeSnapshot(doc.as<JsonVariantConst>(), &s, &report, "/d");
        if (!report.ok()) {
            *firstIssue = std::string(errorCodeName(report.issues()[0].code)) + " " + report.issues()[0].path;
        }
        return ok;
    };
    std::string issue;
    EXPECT_TRUE(decode(R"({"platform":"sonos","supported_features":0,"services":[]})", &issue));
    EXPECT_FALSE(decode(R"({"platform":"sonos","supported_features":0,"services":[],"x":1})", &issue));
    EXPECT_EQ(issue, "unknown_field /d/x");
    EXPECT_FALSE(decode(R"({"platform":"sonos","supported_features":0})", &issue));
    EXPECT_EQ(issue, "required /d/services");
    // A service of another integration (or of the entity's domain) can't be smuggled in.
    EXPECT_FALSE(decode(R"({"platform":"sonos","supported_features":0,
        "services":[{"service":"media_player.play_media","fields":[]}]})", &issue));
    EXPECT_EQ(issue, "invalid_command_id /d/services/0/service");
    // Target-like field names are refused.
    EXPECT_FALSE(decode(R"({"platform":"sonos","supported_features":0,"services":[{"service":"sonos.x",
        "fields":[{"name":"entity_id","type":"enum","required":true,"options":["media_player.all"]}]}]})", &issue));
    EXPECT_EQ(issue, "invalid_id /d/services/0/fields/0/name");
    // One field of a sonos.x service, then the whole snapshot.
    const auto field = [](const std::string &fragment) {
        return R"({"platform":"sonos","supported_features":0,"services":[{"service":"sonos.x","fields":[{)" +
               fragment + "}]}]}";
    };
    // Inverted ranges, too many fields, unknown types.
    EXPECT_FALSE(decode(field(R"("name":"a","type":"int","required":true,"minimum":5,"maximum":1)"), &issue));
    EXPECT_EQ(issue, "param_out_of_range /d/services/0/fields/0/maximum");
    EXPECT_FALSE(decode(field(R"("name":"a","type":"string","required":true)"), &issue));
    EXPECT_EQ(issue, "wrong_type /d/services/0/fields/0/type");
    // A default must fit its param.
    const std::string intField = R"("name":"a","type":"int","required":false,"minimum":1,"maximum":5,)";
    EXPECT_TRUE(decode(field(intField + R"("default":3)"), &issue));
    for (const char *bad : {R"("default":9)", R"("default":2.5)", R"("default":"3")", R"("default":true)"}) {
        EXPECT_FALSE(decode(field(intField + bad), &issue)) << bad;
        EXPECT_EQ(issue, "param_out_of_range /d/services/0/fields/0/default") << bad;
    }
    EXPECT_FALSE(decode(field(R"("name":"a","type":"enum","required":false,"options":["x"],"default":"y")"), &issue));
    EXPECT_EQ(issue, "param_out_of_range /d/services/0/fields/0/default");
    // A fan's attributes.
    EXPECT_TRUE(decode(R"({"platform":"","supported_features":57,"services":[],"preset_modes":["sleep"],
        "percentage_step":33})", &issue));
    for (const char *bad : {"0", "-1", "101"}) {
        EXPECT_FALSE(decode(std::string(R"({"platform":"","supported_features":1,"services":[],"percentage_step":)") +
                                bad + "}",
                            &issue))
            << bad;
        EXPECT_EQ(issue, "param_out_of_range /d/percentage_step") << bad;
    }
}

TEST(HaSnapshot, FanRoundTripsAndBuildsItsCommands) {
    EntityInfo fan;
    fan.entityId = "fan.box_fan";
    fan.supportedFeatures = 61;
    fan.presetModes = {"auto"};
    fan.percentageStep = 14.285714285714286;
    const CapabilitySnapshot snapshot = makeSnapshot(fan, "", ServiceDirectory::parse("{}", {}));
    CapabilitySnapshot decoded;
    ASSERT_TRUE(roundTrip(snapshot, &decoded));
    EXPECT_EQ(decoded.presetModes, std::vector<std::string>{"auto"});
    EXPECT_EQ(decoded.percentageStep, 14) << "stored rounded down";
    const EntityCapabilities caps = EntityCapabilities::fromSnapshot(fan.entityId, decoded);
    const CommandCatalog catalog = caps.catalog();
    ASSERT_GE(catalog.find("fan.set_preset_mode"), 0);
    EXPECT_EQ(catalog.items[catalog.find("fan.set_percentage")].params[0].step, 14.0f);
    EXPECT_GE(catalog.find("fan.set_direction"), 0);
}

TEST(HaSnapshot, CommandsAreRecheckedWhenBuilt) {
    // A snapshot built in code (not decoded) still can't offer unsafe services.
    CapabilitySnapshot s;
    s.platform = "sonos";
    SnapshotService wrongPlatform{"denonavr.x", "", {}};
    SnapshotService domainOwn{"media_player.play_media", "", {}};
    SnapshotParam target;
    target.name = "entity_id";
    target.type = SnapshotParamType::Bool;
    SnapshotService targetField{"sonos.y", "", {target}};
    SnapshotService fine{"sonos.z", "", {}};
    s.services = {wrongPlatform, domainOwn, targetField, fine};
    const EntityCapabilities caps = EntityCapabilities::fromSnapshot("switch.kitchen", s);
    const CommandCatalog catalog = caps.catalog();
    EXPECT_LT(catalog.find("denonavr.x"), 0);
    EXPECT_LT(catalog.find("media_player.play_media"), 0);
    EXPECT_LT(catalog.find("sonos.y"), 0);
    EXPECT_GE(catalog.find("sonos.z"), 0);
    EXPECT_GE(catalog.find("switch.turn_on"), 0);
    // An invalid entity id: nothing at all.
    EXPECT_EQ(EntityCapabilities::fromSnapshot("switch.all", s).catalog().count, 0);
}

// Whatever HA reports, the snapshot registration stores must reload:
// makeSnapshot applies the decoder's rules (a mismatch was a 500 on commit).
TEST(HaSnapshot, OneBuiltFromMessyAttributesAlwaysDecodes) {
    const ServiceDirectory dir = ServiceDirectory::parse(kServices, {"sonos"});
    EntityInfo entity;
    entity.entityId = "media_player.messy";
    entity.supportedFeatures = media_player_feature::kSelectSource | media_player_feature::kSelectSoundMode;
    for (int i = 0; i < 200; i++) entity.sourceList.push_back("Source " + std::to_string(i % 150));  // dups, >128
    entity.sourceList.push_back("");
    entity.sourceList.push_back(std::string(65, 'x'));
    entity.soundModeList = {"MOVIE", "MOVIE", ""};
    entity.supportedColorModes = {std::string(100, 'c'), "xy"};
    entity.minColorTempKelvin = -5;
    entity.maxColorTempKelvin = 200000;
    entity.presetModes = {"auto", "auto", ""};
    entity.percentageStep = 250.0;

    for (const std::string &platform : std::vector<std::string>{"sonos", "Bad-Platform", std::string(40, 'p'), ""}) {
        ServiceDirectory::Offer offered;
        const CapabilitySnapshot snapshot = makeSnapshot(entity, platform, dir, &offered);
        CapabilitySnapshot decoded;
        ASSERT_TRUE(roundTrip(snapshot, &decoded)) << platform;
        EXPECT_GT(offered.droppedOptions, 0u);
        EXPECT_LE(snapshot.sourceList.size(), limits::kMaxEnumOptions);
        if (platform != "sonos") {
            EXPECT_TRUE(snapshot.platform.empty()) << "an invalid platform is dropped";
            EXPECT_TRUE(snapshot.services.empty()) << "no services without a valid platform";
        }
    }
}
