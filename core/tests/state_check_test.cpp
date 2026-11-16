// The state check's rule (docs/design/state-checks.md): power, plus every
// field matched to the attribute of the same name, all or nothing.
#include <gtest/gtest.h>

#include <string>

#include "hubbo/integrations/state_check.h"

using hubbo::ha::ServiceCall;
using hubbo::ha::ServiceField;
using hubbo::ha::ServiceValue;
using hubbo::integrations::alreadySet;

namespace {

ServiceCall call(const char *domain, const char *service) {
    ServiceCall c;
    c.domain = domain;
    c.service = service;
    c.entityId = std::string(domain) + ".x";
    return c;
}

ServiceValue text(const char *s) {
    ServiceValue v;
    v.kind = ServiceValue::Kind::String;
    v.string = s;
    return v;
}

ServiceValue number(double n) {
    ServiceValue v;
    v.kind = ServiceValue::Kind::Number;
    v.number = n;
    return v;
}

ServiceValue integer(int32_t n) {
    ServiceValue v;
    v.kind = ServiceValue::Kind::Int;
    v.integer = n;
    return v;
}

ServiceValue boolean(bool b) {
    ServiceValue v;
    v.kind = ServiceValue::Kind::Bool;
    v.boolean = b;
    return v;
}

ServiceCall with(ServiceCall c, const char *key, ServiceValue value) {
    c.data.push_back(ServiceField{key, std::move(value)});
    return c;
}

}  // namespace

TEST(StateCheck, PowerIsOnlyARealPowerState) {
    const ServiceCall on = call("media_player", "turn_on");
    for (const char *state : {"on", "playing", "idle", "paused", "buffering"}) {
        EXPECT_TRUE(alreadySet(on, std::string(R"({"s":")") + state + R"(","a":{}})")) << state;
    }
    for (const char *state : {"off", "standby", "unavailable", "unknown", "heat"}) {
        EXPECT_FALSE(alreadySet(on, std::string(R"({"s":")") + state + R"(","a":{}})")) << state;
    }
    const ServiceCall off = call("light", "turn_off");
    EXPECT_TRUE(alreadySet(off, R"({"s":"off","a":{}})"));
    EXPECT_TRUE(alreadySet(off, R"({"s":"standby","a":{}})"));
    for (const char *state : {"on", "playing", "unavailable", "unknown"}) {
        EXPECT_FALSE(alreadySet(off, std::string(R"({"s":")") + state + R"(","a":{}})")) << state;
    }
}

TEST(StateCheck, AStatelessEntityAlwaysRuns) {
    // A scene's state is when it last ran: turning it on always runs.
    const ServiceCall scene = call("scene", "turn_on");
    EXPECT_FALSE(alreadySet(scene, R"({"s":"2026-11-15T10:00:00+00:00","a":{}})"));
    EXPECT_FALSE(alreadySet(scene, R"({"s":"unknown","a":{}})"));
    // A climate's state is its mode, not its power; only "off" is.
    const ServiceCall heat = call("climate", "turn_on");
    for (const char *state : {"heat", "cool", "auto", "heat_cool"}) {
        EXPECT_FALSE(alreadySet(heat, std::string(R"({"s":")") + state + R"(","a":{}})")) << state;
    }
    EXPECT_TRUE(alreadySet(call("climate", "turn_off"), R"({"s":"off","a":{}})"));
    // A light's playing-like states don't exist; a switch's on does.
    EXPECT_FALSE(alreadySet(call("light", "turn_on"), R"({"s":"idle","a":{}})"));
    EXPECT_TRUE(alreadySet(call("switch", "turn_on"), R"({"s":"on","a":{}})"));
}

TEST(StateCheck, EveryFieldMatchesItsAttribute) {
    const ServiceCall source = with(call("media_player", "select_source"), "source", text("Game"));
    EXPECT_TRUE(alreadySet(source, R"({"s":"on","a":{"source":"Game"}})"));
    EXPECT_FALSE(alreadySet(source, R"({"s":"on","a":{"source":"TV"}})"));
    EXPECT_FALSE(alreadySet(source, R"({"s":"off","a":{}})"));  // off: no input reported
    EXPECT_FALSE(alreadySet(source, R"({"s":"unavailable","a":{"source":"Game"}})"));
    EXPECT_FALSE(alreadySet(source, R"({"s":"unknown","a":{"source":"Game"}})"));

    // Numbers within a tiny tolerance, ints against floats; bools exactly.
    const ServiceCall volume = with(call("media_player", "volume_set"), "volume_level", number(0.35));
    EXPECT_TRUE(alreadySet(volume, R"({"s":"on","a":{"volume_level":0.35000000001}})"));
    EXPECT_FALSE(alreadySet(volume, R"({"s":"on","a":{"volume_level":0.36}})"));
    EXPECT_FALSE(alreadySet(volume, R"({"s":"on","a":{"volume_level":"0.35"}})"));
    EXPECT_TRUE(alreadySet(with(call("fan", "set_percentage"), "percentage", integer(50)),
                           R"({"s":"on","a":{"percentage":50.0}})"));
    const ServiceCall mute = with(call("media_player", "volume_mute"), "is_volume_muted", boolean(true));
    EXPECT_TRUE(alreadySet(mute, R"({"s":"on","a":{"is_volume_muted":true}})"));
    EXPECT_FALSE(alreadySet(mute, R"({"s":"on","a":{"is_volume_muted":false}})"));
    EXPECT_FALSE(alreadySet(mute, R"({"s":"on","a":{"is_volume_muted":1}})"));
}

TEST(StateCheck, AllOrNothing) {
    // light.turn_on at 128: only when it's on at 128.
    const ServiceCall dim = with(call("light", "turn_on"), "brightness", integer(128));
    EXPECT_TRUE(alreadySet(dim, R"({"s":"on","a":{"brightness":128}})"));
    EXPECT_FALSE(alreadySet(dim, R"({"s":"on","a":{"brightness":255}})"));
    EXPECT_FALSE(alreadySet(dim, R"({"s":"off","a":{"brightness":128}})"));
    // A field with no attribute of its name runs.
    EXPECT_FALSE(alreadySet(with(dim, "transition", integer(2)), R"({"s":"on","a":{"brightness":128}})"));
    EXPECT_FALSE(alreadySet(with(call("light", "turn_on"), "brightness_pct", integer(50)),
                            R"({"s":"on","a":{"brightness":128}})"));
    // Lists and objects are never compared.
    EXPECT_FALSE(alreadySet(with(call("light", "turn_on"), "rgb_color", text("[255,0,0]")),
                            R"({"s":"on","a":{"rgb_color":[255,0,0]}})"));
}

TEST(StateCheck, WhatSetsNoStateAlwaysRuns) {
    const char *on = R"({"s":"on","a":{"source":"Game"}})";
    for (const char *service : {"toggle", "volume_up", "media_play_pause", "send_command"}) {
        EXPECT_FALSE(alreadySet(call("media_player", service), on)) << service;
    }
    // A Home Assistant action (raw data) is never checked.
    ServiceCall raw = call("media_player", "turn_on");
    raw.rawData = "{}";
    EXPECT_FALSE(alreadySet(raw, on));
    // Anything unreadable runs.
    EXPECT_FALSE(alreadySet(call("media_player", "turn_on"), "not json"));
    EXPECT_FALSE(alreadySet(call("media_player", "turn_on"), R"({"a":{}})"));
}
