// Ids, names, error codes, triggers and typed params.
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <set>
#include <string>

#include "hubbo/domain/error.h"
#include "hubbo/domain/ids.h"
#include "hubbo/domain/params.h"
#include "hubbo/domain/system.h"
#include "hubbo/domain/trigger.h"

using namespace hubbo;
using remote::Button;
using remote::EventKind;

TEST(Ids, AcceptsWellFormedIds) {
    for (const char *id : {"a", "ble", "ha_main", "dev_72", "act_watch_apple_tv", "x1"}) {
        EXPECT_TRUE(isValidId(id)) << id;
    }
    EXPECT_TRUE(isValidId(std::string(32, 'a')));
}

TEST(Ids, RejectsMalformedIds) {
    for (const char *id : {"", "1dev", "_dev", "-dev", "Dev", "dev 72", "dev.72", "dév", "dev/72", "dev\n", "watch-apple-tv"}) {
        EXPECT_FALSE(isValidId(id)) << id;
    }
    EXPECT_FALSE(isValidId(std::string(33, 'a')));
    EXPECT_FALSE(isValidId(std::string("dev\0x", 5)));
}

TEST(Ids, CommandIds) {
    for (const char *id : {"wait", "key_down", "media_player.volume_up", "light.turn_on2"}) {
        EXPECT_TRUE(isValidCommandId(id)) << id;
    }
    for (const char *id : {"", ".wait", "wait.", "a..b", "a.b.c", "Media.x", "a.1b", "a-b", "a b", "a/b"}) {
        EXPECT_FALSE(isValidCommandId(id)) << id;
    }
    EXPECT_TRUE(isValidCommandId(std::string(32, 'a') + "." + std::string(32, 'b')));
    EXPECT_FALSE(isValidCommandId(std::string(33, 'a')));
    EXPECT_FALSE(isValidCommandId(std::string(32, 'a') + "." + std::string(33, 'b')));
}

TEST(Ids, Names) {
    EXPECT_TRUE(isValidName("Watch Apple TV"));
    EXPECT_TRUE(isValidName("Télé du salon"));
    EXPECT_FALSE(isValidName(""));
    EXPECT_FALSE(isValidName("tab\tname"));
    EXPECT_FALSE(isValidName(std::string(65, 'n')));
}

TEST(FixedString, RefusesRatherThanTruncates) {
    FixedString<4> s;
    EXPECT_TRUE(s.assign("abcd"));
    EXPECT_EQ(s.view(), "abcd");
    EXPECT_FALSE(s.assign("abcde"));
    EXPECT_TRUE(s.empty());
    EXPECT_STREQ(s.c_str(), "");
}

TEST(ErrorCodes, EveryCodeHasAUniqueSnakeCaseName) {
    std::set<std::string> names;
    for (size_t i = 0; i < static_cast<size_t>(ErrorCode::Count); i++) {
        const std::string name = errorCodeName(static_cast<ErrorCode>(i));
        EXPECT_TRUE(names.insert(name).second) << name;
        for (char c : name) EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_') << name;
    }
    EXPECT_STREQ(errorCodeName(ErrorCode::Ok), "ok");
    EXPECT_STREQ(errorCodeName(ErrorCode::QueueFull), "queue_full");
}

TEST(Trigger, RoundTripsEveryButtonAndKind) {
    for (size_t b = 0; b < remote::kButtonCount; b++) {
        for (size_t k = 0; k < remote::kEventKindCount; k++) {
            const Trigger t{static_cast<Button>(b), static_cast<EventKind>(k)};
            Trigger parsed;
            ASSERT_TRUE(parseTrigger(formatTrigger(t), &parsed)) << formatTrigger(t);
            EXPECT_EQ(parsed, t);
            EXPECT_LT(triggerIndex(t), kTriggerCount);
        }
    }
    EXPECT_EQ(formatTrigger({Button::Up, EventKind::Down}), "up:down");
    EXPECT_EQ(formatTrigger({Button::Info, EventKind::Hold}), "info:hold");
}

TEST(Trigger, RejectsMalformed) {
    Trigger t;
    for (const char *s : {"", "up", "up:", ":down", "up:down:x", "UP:down", "up:press", "nope:down", " up:down",
                          "up :down", "up:Down", "up::down", "up:double"}) {
        EXPECT_FALSE(parseTrigger(s, &t)) << s;
    }
    EXPECT_FALSE(parseTrigger(std::string(100, 'a') + ":down", &t));
}

namespace {

constexpr const char *kColors[] = {"red", "green"};
constexpr ParamSpec kSpecs[] = {
    ParamSpec::integer("count", -5, 10),
    ParamSpec::number("level", 0.0f, 1.0f, false, 0.5f),
    ParamSpec::boolean("flag", false, true),
    ParamSpec::enumeration("color", kColors, 2, false, 1),
};
constexpr CommandSpec kCommand{"test.cmd", kSpecs, 4};

ParamValue coerce(const ParamSpec &spec, const ParamLiteral &lit, ErrorCode expected) {
    ParamValue v;
    EXPECT_EQ(coerceParam(spec, lit, &v), expected);
    return v;
}

}  // namespace

TEST(Params, IntBoundsAndTypes) {
    const ParamSpec &spec = kSpecs[0];
    EXPECT_EQ(coerce(spec, ParamLiteral::ofInt(10), ErrorCode::Ok).integer, 10);
    EXPECT_EQ(coerce(spec, ParamLiteral::ofInt(-5), ErrorCode::Ok).integer, -5);
    EXPECT_EQ(coerce(spec, ParamLiteral::ofNumber(3.0), ErrorCode::Ok).integer, 3);
    coerce(spec, ParamLiteral::ofInt(11), ErrorCode::ParamOutOfRange);
    coerce(spec, ParamLiteral::ofInt(int64_t{1} << 40), ErrorCode::ParamOutOfRange);
    coerce(spec, ParamLiteral::ofNumber(3.5), ErrorCode::InvalidParamType);
    coerce(spec, ParamLiteral::ofNumber(1e300), ErrorCode::ParamOutOfRange);
    coerce(spec, ParamLiteral::ofString("3"), ErrorCode::InvalidParamType);
    coerce(spec, ParamLiteral::ofBool(true), ErrorCode::InvalidParamType);
    coerce(spec, ParamLiteral::null(), ErrorCode::InvalidParamType);
}

TEST(Params, NumberBoundsAndNonFinite) {
    const ParamSpec &spec = kSpecs[1];
    EXPECT_FLOAT_EQ(coerce(spec, ParamLiteral::ofNumber(0.25), ErrorCode::Ok).number, 0.25f);
    EXPECT_FLOAT_EQ(coerce(spec, ParamLiteral::ofInt(1), ErrorCode::Ok).number, 1.0f);
    coerce(spec, ParamLiteral::ofNumber(1.01), ErrorCode::ParamOutOfRange);
    coerce(spec, ParamLiteral::ofNumber(-0.01), ErrorCode::ParamOutOfRange);
    coerce(spec, ParamLiteral::ofNumber(std::nan("")), ErrorCode::ParamOutOfRange);
    coerce(spec, ParamLiteral::ofNumber(std::numeric_limits<double>::infinity()), ErrorCode::ParamOutOfRange);
    coerce(spec, ParamLiteral::ofString("0.5"), ErrorCode::InvalidParamType);
}

TEST(Params, BoolAndEnum) {
    EXPECT_TRUE(coerce(kSpecs[2], ParamLiteral::ofBool(true), ErrorCode::Ok).asBool());
    coerce(kSpecs[2], ParamLiteral::ofInt(1), ErrorCode::InvalidParamType);
    EXPECT_EQ(coerce(kSpecs[3], ParamLiteral::ofString("green"), ErrorCode::Ok).asOption(), 1);
    coerce(kSpecs[3], ParamLiteral::ofString("GREEN"), ErrorCode::InvalidEnumValue);
    coerce(kSpecs[3], ParamLiteral::ofString(""), ErrorCode::InvalidEnumValue);
    coerce(kSpecs[3], ParamLiteral::ofInt(0), ErrorCode::InvalidParamType);
}

TEST(Params, ResolveFillsSpecOrderAndDefaults) {
    ParamValue out[limits::kMaxParams];
    std::string bad;
    ASSERT_EQ(resolveParams(kCommand, {{"color", ParamLiteral::ofString("red")}, {"count", ParamLiteral::ofInt(2)}},
                            out, &bad),
              ErrorCode::Ok);
    EXPECT_EQ(out[0].integer, 2);
    EXPECT_FLOAT_EQ(out[1].number, 0.5f);  // default
    EXPECT_TRUE(out[2].asBool());          // default
    EXPECT_EQ(out[3].asOption(), 0);
}

TEST(Params, ResolveReportsTheOffendingParam) {
    ParamValue out[limits::kMaxParams];
    std::string bad;
    EXPECT_EQ(resolveParams(kCommand, {}, out, &bad), ErrorCode::MissingParam);
    EXPECT_EQ(bad, "count");
    EXPECT_EQ(resolveParams(kCommand, {{"count", ParamLiteral::ofInt(1)}, {"bogus", ParamLiteral::ofInt(1)}}, out, &bad),
              ErrorCode::UnknownParam);
    EXPECT_EQ(bad, "bogus");
    EXPECT_EQ(resolveParams(kCommand, {{"count", ParamLiteral::ofInt(1)}, {"count", ParamLiteral::ofInt(2)}}, out, &bad),
              ErrorCode::DuplicateParam);
    EXPECT_EQ(bad, "count");
    EXPECT_EQ(resolveParams(kCommand, {{"count", ParamLiteral::ofInt(99)}}, out, &bad), ErrorCode::ParamOutOfRange);
    EXPECT_EQ(bad, "count");
    std::vector<ParamInput> five(5, {"count", ParamLiteral::ofInt(1)});
    EXPECT_EQ(resolveParams(kCommand, five, out, &bad), ErrorCode::TooManyParams);
}

TEST(Params, ActivityRefNeedsAResolver) {
    struct Refs : RefResolver {
        int activityIndex(std::string_view id) const override { return id == "watch" ? 3 : -1; }
    } refs;
    const ParamSpec spec = ParamSpec::activityRef("activity_id");
    ParamValue v;
    EXPECT_EQ(coerceParam(spec, ParamLiteral::ofString("watch"), &v, &refs), ErrorCode::Ok);
    EXPECT_EQ(v.integer, 3);
    EXPECT_EQ(coerceParam(spec, ParamLiteral::ofString("nope"), &v, &refs), ErrorCode::UnknownActivity);
    EXPECT_EQ(coerceParam(spec, ParamLiteral::ofInt(3), &v, &refs), ErrorCode::InvalidParamType);
    EXPECT_EQ(coerceParam(spec, ParamLiteral::ofString("watch"), &v, nullptr), ErrorCode::Internal);
}

TEST(SystemCatalog, WaitIsBounded) {
    const CommandCatalog catalog = systemCatalog();
    ASSERT_EQ(catalog.find("wait"), static_cast<int>(SystemCommand::Wait));
    ASSERT_EQ(catalog.find("activate"), static_cast<int>(SystemCommand::Activate));
    ASSERT_EQ(catalog.find("off"), static_cast<int>(SystemCommand::Off));
    EXPECT_EQ(catalog.find("reboot"), -1);
    ParamValue out[limits::kMaxParams];
    std::string bad;
    const CommandSpec &wait = catalog.items[0];
    EXPECT_EQ(resolveParams(wait, {{"duration_ms", ParamLiteral::ofInt(60000)}}, out, &bad), ErrorCode::Ok);
    EXPECT_EQ(resolveParams(wait, {{"duration_ms", ParamLiteral::ofInt(60001)}}, out, &bad), ErrorCode::ParamOutOfRange);
    EXPECT_EQ(resolveParams(wait, {{"duration_ms", ParamLiteral::ofInt(-1)}}, out, &bad), ErrorCode::ParamOutOfRange);
    EXPECT_EQ(resolveParams(wait, {}, out, &bad), ErrorCode::MissingParam);
}
