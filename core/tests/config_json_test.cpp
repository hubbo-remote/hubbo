// Config <-> JSON: round trips, strict shape errors with JSON Pointer paths,
// bounded parsing and schema versions.
#include <gtest/gtest.h>

#include <cstring>

#include "fakes.h"
#include "fixtures.h"
#include "hubbo/config/json.h"

using namespace hubbo;
using namespace hubbo::config;

namespace {

ValidationReport decodeActivityText(const char *text, ActivityShape shape, ActivitySpec *out = nullptr) {
    JsonDocument doc;
    EXPECT_EQ(parseJson(text, doc, kMaxRequestBytes, kRequestNestingLimit), ErrorCode::Ok) << text;
    ActivitySpec scratch;
    ValidationReport report;
    decodeActivity(doc.as<JsonVariantConst>(), shape, out != nullptr ? out : &scratch, &report);
    return report;
}

const char *kMinimalActivity = R"({"name":"Movie","startup":[],"shutdown":[],"bindings":{}})";

}  // namespace

TEST(ConfigJson, FixtureRoundTrips) {
    const ConfigSpec original = sim::fixtureConfig();
    const std::string text = serializeConfig(original);
    ConfigSpec parsed;
    const ParseResult result = parseConfig(text, &parsed);
    ASSERT_EQ(result.code, ErrorCode::Ok) << (result.report.ok() ? "" : result.report.issues()[0].path);
    EXPECT_EQ(result.schemaVersion, kConfigFileVersion);
    EXPECT_EQ(serializeConfig(parsed), text);  // stable
    ASSERT_EQ(parsed.activities.size(), 2u);
    EXPECT_EQ(parsed.activities[0].id, "act_watch_apple_tv");
    EXPECT_EQ(parsed.devices[1].provider.entityId, "media_player.family_room_lg_tv");
    EXPECT_EQ(parsed.integrations[2].url, "http://homeassistant.local:8123");
    EXPECT_EQ(parsed.globalBindingsRevision, original.globalBindingsRevision);
    // Params survive (wait duration, enum source, bool).
    const auto &startup = parsed.activities[0].startup;
    ASSERT_EQ(startup[2].params.size(), 1u);
    EXPECT_EQ(startup[2].params[0].name, "duration_ms");
    EXPECT_EQ(startup[2].params[0].value.integer, 1000);
    EXPECT_EQ(startup[3].params[0].value.string, "HDMI 1");
    // And the parsed config is exactly as valid as the original.
    sim::FakeAdapters fakes;
    ValidationReport r;
    EXPECT_NE(compileConfig(parsed, fakes.set, &r), nullptr);
}

TEST(ConfigJson, ParamTypesSurvive) {
    ActivitySpec a;
    const auto report = decodeActivityText(
        R"({"name":"A","startup":[{"integration_id":"x","device_id":null,"command_id":"c",
            "params":{"i":3,"f":0.5,"b":true,"s":"HDMI 1"}}],"shutdown":[],"bindings":{}})",
        ActivityShape::Create, &a);
    ASSERT_TRUE(report.ok());
    const auto &p = a.startup[0].params;
    ASSERT_EQ(p.size(), 4u);
    EXPECT_EQ(p[0].value.kind, ParamLiteral::Kind::Int);
    EXPECT_EQ(p[1].value.kind, ParamLiteral::Kind::Number);
    EXPECT_EQ(p[2].value.kind, ParamLiteral::Kind::Bool);
    EXPECT_EQ(p[3].value.string, "HDMI 1");
    EXPECT_FALSE(a.startup[0].deviceId.has_value());
}

TEST(ConfigJson, ShapesPerOperation) {
    EXPECT_TRUE(decodeActivityText(kMinimalActivity, ActivityShape::Create).ok());
    // Create takes no id/revision; Replace needs a revision; Stored needs id + schema_version.
    EXPECT_TRUE(decodeActivityText(R"({"id":"act_x","name":"A","startup":[],"shutdown":[],"bindings":{}})",
                                   ActivityShape::Create)
                    .has(ErrorCode::UnknownField, "/id"));
    EXPECT_TRUE(decodeActivityText(kMinimalActivity, ActivityShape::Replace).has(ErrorCode::Required, "/revision"));
    EXPECT_TRUE(decodeActivityText(kMinimalActivity, ActivityShape::Validate).ok());
    const auto stored = decodeActivityText(kMinimalActivity, ActivityShape::Stored);
    EXPECT_TRUE(stored.has(ErrorCode::Required, "/id"));
    EXPECT_TRUE(stored.has(ErrorCode::Required, "/schema_version"));
    EXPECT_TRUE(decodeActivityText(
                    R"({"name":"A","startup":[],"shutdown":[],"bindings":{},"schema_version":2,"revision":1})",
                    ActivityShape::Replace)
                    .has(ErrorCode::UnsupportedSchema, "/schema_version"));
}

TEST(ConfigJson, StrictShapeErrors) {
    const auto r = decodeActivityText(
        R"({"name":7,"startup":{},"shutdown":[{"integration_id":"x","command_id":"c","params":{},"extra":1}],
            "bindings":{"up:down":[{"integration_id":"x","device_id":5,"command_id":"c","params":[]}]},
            "colour":"red"})",
        ActivityShape::Create);
    EXPECT_TRUE(r.has(ErrorCode::WrongType, "/name"));
    EXPECT_TRUE(r.has(ErrorCode::WrongType, "/startup"));
    EXPECT_TRUE(r.has(ErrorCode::Required, "/shutdown/0/device_id"));
    EXPECT_TRUE(r.has(ErrorCode::UnknownField, "/shutdown/0/extra"));
    EXPECT_TRUE(r.has(ErrorCode::WrongType, "/bindings/up:down/0/device_id"));
    EXPECT_TRUE(r.has(ErrorCode::WrongType, "/bindings/up:down/0/params"));
    EXPECT_TRUE(r.has(ErrorCode::UnknownField, "/colour"));
}

TEST(ConfigJson, Bounds) {
    const std::string longName(limits::kMaxNameLength + 1, 'n');
    EXPECT_TRUE(decodeActivityText(("{\"name\":\"" + longName + R"(","startup":[],"shutdown":[],"bindings":{}})").c_str(),
                                   ActivityShape::Create)
                    .has(ErrorCode::TooLong, "/name"));
    std::string seq = "[";
    for (size_t i = 0; i <= limits::kMaxSequenceLength; i++) {
        seq += std::string(i ? "," : "") + R"({"integration_id":"system","device_id":null,"command_id":"off","params":{}})";
    }
    seq += "]";
    EXPECT_TRUE(decodeActivityText(("{\"name\":\"A\",\"startup\":" + seq + R"(,"shutdown":[],"bindings":{}})").c_str(),
                                   ActivityShape::Create)
                    .has(ErrorCode::TooLong, "/startup"));
    const auto params = decodeActivityText(
        R"({"name":"A","startup":[{"integration_id":"x","device_id":null,"command_id":"c",
            "params":{"a":1,"b":2,"c":3,"d":4,"e":5}}],"shutdown":[],"bindings":{}})",
        ActivityShape::Create);
    EXPECT_TRUE(params.has(ErrorCode::TooLong, "/startup/0/params"));
    const auto nested = decodeActivityText(
        R"({"name":"A","startup":[{"integration_id":"x","device_id":null,"command_id":"c",
            "params":{"a":{"entity_id":"all"}}}],"shutdown":[],"bindings":{}})",
        ActivityShape::Create);
    EXPECT_TRUE(nested.has(ErrorCode::WrongType, "/startup/0/params/a"));
}

TEST(ConfigJson, ParseLimits) {
    JsonDocument doc;
    EXPECT_EQ(parseJson("", doc, 100, 8), ErrorCode::MalformedJson);
    EXPECT_EQ(parseJson("{\"a\":", doc, 100, 8), ErrorCode::MalformedJson);
    EXPECT_EQ(parseJson("not json", doc, 100, 8), ErrorCode::MalformedJson);
    EXPECT_EQ(parseJson(std::string(200, ' ') + "{}", doc, 100, 8), ErrorCode::MalformedJson);  // too big
    EXPECT_EQ(parseJson("[[[[[[[[[1]]]]]]]]]", doc, 100, 8), ErrorCode::MalformedJson);        // 9 deep
    EXPECT_EQ(parseJson("[[[[[[[[1]]]]]]]]", doc, 100, 8), ErrorCode::Ok);                     // 8 deep
}

TEST(ConfigJson, Devices) {
    JsonDocument doc;
    ASSERT_EQ(parseJson(R"({"name":"TV","integration_id":"ha_main","kind":"tv",
                            "provider":{"type":"home_assistant","entity_id":"media_player.tv"}})",
                        doc, kMaxRequestBytes, kRequestNestingLimit),
              ErrorCode::Ok);
    DeviceSpec d;
    ValidationReport r;
    EXPECT_TRUE(decodeDevice(doc.as<JsonVariantConst>(), DeviceShape::Register, &d, &r));
    EXPECT_EQ(providerRef(d), "media_player.tv");

    ASSERT_EQ(parseJson(R"({"name":"TV","integration_id":"ha_main","kind":"Fridge",
                            "provider":{"type":"zigbee"}})",
                        doc, kMaxRequestBytes, kRequestNestingLimit),
              ErrorCode::Ok);
    r = {};
    EXPECT_FALSE(decodeDevice(doc.as<JsonVariantConst>(), DeviceShape::Register, &d, &r));
    EXPECT_TRUE(r.has(ErrorCode::UnknownKind, "/kind"));  // malformed: can't be a kind
    EXPECT_TRUE(r.has(ErrorCode::WrongType, "/provider/type"));
    // An update's kind, malformed or too long: unknown_kind as well.
    for (const char *body : {R"({"kind":"Not A Kind","revision":1})",
                             R"({"kind":"a_kind_far_longer_than_any_id_may_be","revision":1})"}) {
        ASSERT_EQ(parseJson(body, doc, kMaxRequestBytes, kRequestNestingLimit), ErrorCode::Ok);
        DeviceUpdate bad;
        r = {};
        EXPECT_FALSE(decodeDeviceUpdate(doc.as<JsonVariantConst>(), &bad, &r)) << body;
        EXPECT_TRUE(r.has(ErrorCode::UnknownKind, "/kind")) << body;
    }

    ASSERT_EQ(parseJson(R"({"name":"Den","revision":3})", doc, kMaxRequestBytes, kRequestNestingLimit), ErrorCode::Ok);
    DeviceUpdate u;
    r = {};
    EXPECT_TRUE(decodeDeviceUpdate(doc.as<JsonVariantConst>(), &u, &r));
    EXPECT_EQ(*u.name, "Den");
    EXPECT_FALSE(u.kind.has_value());
    EXPECT_EQ(u.revision, 3u);
}

TEST(ConfigJson, CapabilitiesAreStoredNotSentByClients) {
    DeviceSpec d = sim::haDevice("dev_lights", "Lights", "light", "light.family_room");
    d.revision = 1;
    d.haCapabilities = ha::CapabilitySnapshot();
    d.haCapabilities->platform = "demo";
    // Stored (config record, backup): kept.
    JsonDocument doc;
    encodeDevice(d, doc.to<JsonObject>());
    ASSERT_TRUE(doc["ha_capabilities"].is<JsonObject>());
    DeviceSpec back;
    ValidationReport r;
    ASSERT_TRUE(decodeDevice(doc.as<JsonVariantConst>(), DeviceShape::Stored, &back, &r));
    ASSERT_TRUE(back.haCapabilities.has_value());
    EXPECT_EQ(back.haCapabilities->platform, "demo");
    // API responses leave it out; requests can't carry it.
    JsonDocument response;
    encodeDevice(d, response.to<JsonObject>(), DeviceDetail::Full);
    EXPECT_TRUE(response["ha_capabilities"].isUnbound());

    ASSERT_EQ(parseJson(R"({"name":"TV","integration_id":"ha_main","ha_capabilities":{},
                            "provider":{"type":"home_assistant","entity_id":"light.x"}})",
                        doc, kMaxRequestBytes, kRequestNestingLimit),
              ErrorCode::Ok);
    r = {};
    EXPECT_FALSE(decodeDevice(doc.as<JsonVariantConst>(), DeviceShape::Register, &back, &r));
    EXPECT_TRUE(r.has(ErrorCode::UnknownField, "/ha_capabilities"));
    ASSERT_EQ(parseJson(R"({"revision":1,"ha_capabilities":{}})", doc, kMaxRequestBytes, kRequestNestingLimit),
              ErrorCode::Ok);
    DeviceUpdate u;
    r = {};
    EXPECT_FALSE(decodeDeviceUpdate(doc.as<JsonVariantConst>(), &u, &r));
    EXPECT_TRUE(r.has(ErrorCode::UnknownField, "/ha_capabilities"));
}

TEST(ConfigJson, IntegrationSecretsAreNotAccepted) {
    JsonDocument doc;
    ASSERT_EQ(parseJson(R"({"id":"ha_main","type":"home_assistant","name":"HA","enabled":true,"revision":1,
                            "settings":{"type":"home_assistant","url":"http://ha:8123","verify_tls":true,
                                        "access_token":"secret"}})",
                        doc, kMaxRequestBytes, kRequestNestingLimit),
              ErrorCode::Ok);
    IntegrationSpec i;
    ValidationReport r;
    EXPECT_FALSE(decodeIntegration(doc.as<JsonVariantConst>(), &i, &r));
    EXPECT_TRUE(r.has(ErrorCode::UnknownField, "/settings/access_token"));

    JsonDocument out;
    i.type = "home_assistant";
    encodeIntegration(i, out.to<JsonObject>());
    std::string text;
    serializeJson(out, text);
    EXPECT_EQ(text.find("token"), std::string::npos);
}

TEST(ConfigJson, MqttSettings) {
    const auto decode = [](const char *settings, IntegrationSpec *out, ValidationReport *r) {
        const std::string text = std::string(R"({"id":"mqtt_main","type":"mqtt","name":"MQTT","enabled":true,)"
                                             R"("revision":1,"settings":)") + settings + "}";
        JsonDocument doc;
        EXPECT_EQ(parseJson(text.c_str(), doc, kMaxRequestBytes, kRequestNestingLimit), ErrorCode::Ok);
        return decodeIntegration(doc.as<JsonVariantConst>(), out, r);
    };
    IntegrationSpec i;
    ValidationReport r;
    ASSERT_TRUE(decode(R"({"type":"mqtt","host":"10.0.0.2","port":1884,"username":"hubbo"})", &i, &r));
    EXPECT_EQ(i.host, "10.0.0.2");
    EXPECT_EQ(i.port, 1884);
    EXPECT_EQ(i.username, "hubbo");

    JsonDocument out;
    encodeIntegration(i, out.to<JsonObject>());
    std::string text;
    serializeJson(out, text);
    EXPECT_NE(text.find(R"("settings":{"type":"mqtt","host":"10.0.0.2","port":1884,"username":"hubbo"})"),
              std::string::npos)
        << text;

    r = {};
    EXPECT_FALSE(decode(R"({"type":"mqtt","host":"h","port":0,"username":""})", &i, &r));
    EXPECT_TRUE(r.has(ErrorCode::ParamOutOfRange, "/settings/port"));
    r = {};
    EXPECT_FALSE(decode(R"({"type":"mqtt","host":"h","port":"1883","username":""})", &i, &r));
    EXPECT_TRUE(r.has(ErrorCode::WrongType, "/settings/port"));
    r = {};
    EXPECT_FALSE(decode(R"({"type":"mqtt","host":"h","port":1883,"username":"","password":"x"})", &i, &r));
    EXPECT_TRUE(r.has(ErrorCode::UnknownField, "/settings/password"));
    // The host and username rules the API's PUT applies hold for a stored
    // record or a backup too.
    r = {};
    EXPECT_FALSE(decode(R"({"type":"mqtt","host":"broker lan","port":1883,"username":""})", &i, &r));
    EXPECT_TRUE(r.has(ErrorCode::WrongType, "/settings/host"));
    r = {};
    EXPECT_FALSE(decode(R"({"type":"mqtt","host":"broker.lan:1883","port":1883,"username":""})", &i, &r));
    EXPECT_TRUE(r.has(ErrorCode::WrongType, "/settings/host"));
    r = {};
    EXPECT_FALSE(decode(R"({"type":"mqtt","host":"h","port":1883,"username":"a\nb"})", &i, &r));
    EXPECT_TRUE(r.has(ErrorCode::WrongType, "/settings/username"));
    r = {};
    EXPECT_TRUE(decode(R"({"type":"mqtt","host":"fe80::1","port":1883,"username":""})", &i, &r));
}

// The cap counts the built-ins ensureBuiltinIntegrations() adds, so a list
// decode accepts never grows past what the next decode (or the compiler)
// accepts.
TEST(ConfigJson, IntegrationsCapCountsTheBuiltins) {
    ConfigSpec c = sim::fixtureConfig();
    ensureBuiltinIntegrations(&c);
    c.integrations.erase(c.integrations.begin() + indexOfId(c.integrations, kMqttIntegrationId));
    for (int n = 2; c.integrations.size() < limits::kMaxIntegrations - 1; n++) {
        IntegrationSpec extra = c.integrations[static_cast<size_t>(indexOfId(c.integrations, "ha_main"))];
        extra.id = "ha_" + std::to_string(n);
        c.integrations.push_back(extra);
    }
    ConfigSpec parsed;
    ASSERT_EQ(parseConfig(serializeConfig(c), &parsed).code, ErrorCode::Ok);
    ensureBuiltinIntegrations(&parsed);  // mqtt_main: at the cap
    ASSERT_EQ(parsed.integrations.size(), limits::kMaxIntegrations);
    for (IntegrationSpec &i : parsed.integrations) i.revision = 1;  // as the service stamps a load
    ConfigSpec again;
    const ParseResult reread = parseConfig(serializeConfig(parsed), &again);
    EXPECT_EQ(reread.code, ErrorCode::Ok)
        << (reread.report.ok() ? "" : reread.report.issues()[0].path + " " + errorCodeName(reread.report.issues()[0].code));

    // One more, still without mqtt_main: adding it would pass the cap.
    IntegrationSpec extra = c.integrations.back();
    extra.id = "ha_99";
    c.integrations.push_back(extra);
    ASSERT_EQ(c.integrations.size(), limits::kMaxIntegrations);
    const ParseResult over = parseConfig(serializeConfig(c), &parsed);
    EXPECT_NE(over.code, ErrorCode::Ok);
    EXPECT_TRUE(over.report.has(ErrorCode::TooLong, "/integrations"));
}

// No retired or runtime fields are tolerated (nothing is released yet): a
// record or backup carrying one is unknown_field like any other.
TEST(ConfigJson, RetiredFieldsAreUnknown) {
    const ConfigSpec c = sim::fixtureConfig();
    struct Case {
        const char *from;
        const char *to;
        const char *field;
    };
    const Case cases[] = {
        {R"("verify_tls":true)", R"("verify_tls":true,"ca_cert_pem":null)", "ca_cert_pem"},
        {R"("provider":{"type":"ble_hid")", R"("provider":{"type":"ble_hid","peer_name":"Family Room")", "peer_name"},
        // Runtime state the API reports is never input either.
        {R"("provider":{"type":"ble_hid")", R"("availability":"available","provider":{"type":"ble_hid")", "availability"},
        {R"("provider":{"type":"ble_hid")", R"("bluetooth":{"state":"connected"},"provider":{"type":"ble_hid")", "bluetooth"},
    };
    for (const Case &k : cases) {
        for (const bool backup : {false, true}) {
            SCOPED_TRACE(std::string(k.field) + (backup ? " in a backup" : " in a record"));
            std::string text = backup ? serializeBackup(c, BackupInfo()) : serializeConfig(c);
            const size_t at = text.find(k.from);
            ASSERT_NE(at, std::string::npos) << text;
            text.replace(at, std::strlen(k.from), k.to);
            ConfigSpec loaded;
            const ParseResult r = backup ? parseBackup(text, &loaded) : parseConfig(text, &loaded);
            EXPECT_EQ(r.code, ErrorCode::ValidationFailed);
            ASSERT_FALSE(r.report.ok());
            EXPECT_EQ(r.report.issues()[0].code, ErrorCode::UnknownField);
        }
    }
}

namespace {

ErrorCode migrateFromZero(JsonDocument &doc, int from) {
    if (from != 0) return ErrorCode::UnsupportedSchema;
    doc["format"] = "hubbo-config";  // v0 had no format marker
    return ErrorCode::Ok;
}

}  // namespace

TEST(ConfigJson, SchemaVersions) {
    ConfigSpec c;
    std::string text = serializeConfig(sim::fixtureConfig());

    std::string newer = text;
    newer.replace(newer.find("\"schema_version\":2"), 18, "\"schema_version\":9");
    EXPECT_EQ(parseConfig(newer, &c).code, ErrorCode::UnsupportedSchema);

    std::string older = text;
    older.replace(older.find("\"schema_version\":2"), 18, "\"schema_version\":0");
    const std::string format = "\"format\":\"hubbo-config\",";
    older.erase(older.find(format), format.size());
    EXPECT_EQ(parseConfig(older, &c).code, ErrorCode::MigrationFailed);  // the built-ins start at 1
    const ParseResult migrated = parseConfig(older, &c, migrateFromZero);
    EXPECT_EQ(migrated.code, ErrorCode::Ok);
    EXPECT_EQ(migrated.schemaVersion, 0);
    EXPECT_EQ(c.activities.size(), 2u);

    EXPECT_EQ(parseConfig("[]", &c).code, ErrorCode::MalformedJson);
    EXPECT_EQ(parseConfig(R"({"format":"hubbo-config"})", &c).code, ErrorCode::ValidationFailed);
}

// ---- a Home Assistant action's call (call_service) -------------------------

namespace {

// Decodes `call` on its own; the issues under /call.
ValidationReport decodeCallText(const std::string &text, HaServiceCallSpec *out = nullptr) {
    JsonDocument doc;
    EXPECT_EQ(parseJson(text, doc, kMaxRequestBytes, kRequestNestingLimit), ErrorCode::Ok) << text;
    HaServiceCallSpec scratch;
    ValidationReport report;
    decodeHaServiceCall(doc.as<JsonVariantConst>(), out != nullptr ? out : &scratch, &report, "/call");
    return report;
}

std::string bindingWith(const std::string &action) {
    return R"({"name":"TV","startup":[],"shutdown":[],"bindings":{"info:tap":[)" + action + "]}}";
}

}  // namespace

TEST(ConfigJson, HaServiceCallShape) {
    HaServiceCallSpec call;
    EXPECT_TRUE(decodeCallText(R"({"service":"webostv.button","data":{ "button" : "INFO" }})", &call).ok());
    EXPECT_EQ(call.service, "webostv.button");
    EXPECT_EQ(call.data, R"({"button":"INFO"})");  // compact
    EXPECT_TRUE(decodeCallText(R"({"service":"webostv.button"})", &call).ok());
    EXPECT_EQ(call.data, "{}");  // the default

    EXPECT_TRUE(decodeCallText(R"({"data":{}})").has(ErrorCode::Required, "/call/service"));
    for (const char *bad : {"webostv", "WebOS.button", "webostv.", ".button", "1tv.button", "webostv.button.x",
                            "webos tv.button"}) {
        EXPECT_TRUE(decodeCallText(std::string(R"({"service":")") + bad + R"("})").has(ErrorCode::Pattern, "/call/service"))
            << bad;
    }
    EXPECT_TRUE(decodeCallText(R"({"service":"a.b","extra":1})").has(ErrorCode::UnknownField, "/call/extra"));
    EXPECT_TRUE(decodeCallText(R"({"service":"a.b","data":[1]})").has(ErrorCode::WrongType, "/call/data"));
    EXPECT_TRUE(decodeCallText(R"({"service":"a.b","data":"{}"})").has(ErrorCode::WrongType, "/call/data"));

    // 4 levels (the data object is 1) are fine; 5 aren't.
    EXPECT_TRUE(decodeCallText(R"({"service":"a.b","data":{"x":{"y":[{"z":1}]}}})").ok());
    EXPECT_TRUE(decodeCallText(R"({"service":"a.b","data":{"x":{"y":[{"z":[]}]}}})").has(ErrorCode::TooDeep, "/call/data"));

    // 1024 bytes compact, and no more.
    const std::string head = R"({"service":"a.b","data":{"k":")";
    const size_t fill = limits::kMaxHaCallDataBytes - std::strlen(R"({"k":""})");
    EXPECT_TRUE(decodeCallText(head + std::string(fill, 'x') + "\"}}").ok());
    EXPECT_TRUE(decodeCallText(head + std::string(fill + 1, 'x') + "\"}}").has(ErrorCode::TooLong, "/call/data"));

    // Target keys at the top level are refused (deeper they're data).
    const ValidationReport targets =
        decodeCallText(R"({"service":"a.b","data":{"entity_id":"x","area_id":"y","nested":{"entity_id":"z"}}})");
    EXPECT_TRUE(targets.has(ErrorCode::ForbiddenParam, "/call/data/entity_id"));
    EXPECT_TRUE(targets.has(ErrorCode::ForbiddenParam, "/call/data/area_id"));
    EXPECT_EQ(targets.issues().size(), 2u);
    for (const char *key : {"device_id", "floor_id", "label_id"}) {
        EXPECT_TRUE(decodeCallText(std::string(R"({"service":"a.b","data":{")") + key + R"(":1}})")
                        .has(ErrorCode::ForbiddenParam, std::string("/call/data/") + key));
    }

    // Templates anywhere, keys included: the first one is reported.
    EXPECT_TRUE(decodeCallText(R"({"service":"a.b","data":{"m":"{{ states('x') }}"}})")
                    .has(ErrorCode::TemplateNotSupported, "/call/data/m"));
    EXPECT_TRUE(decodeCallText(R"({"service":"a.b","data":{"a":{"b":["ok","{% if %}"]}}})")
                    .has(ErrorCode::TemplateNotSupported, "/call/data/a/b/1"));
    EXPECT_TRUE(decodeCallText(R"({"service":"a.b","data":{"{{k}}":1}})")
                    .has(ErrorCode::TemplateNotSupported, "/call/data/{{k}}"));
    EXPECT_TRUE(decodeCallText(R"({"service":"a.b","data":{"m":"{ {not one} }"}})").ok());
}

TEST(ConfigJson, ACallBelongsToCallServiceAlone) {
    const char *call = R"("call":{"service":"webostv.button","data":{"button":"INFO"}})";
    ActivitySpec a;
    EXPECT_TRUE(decodeActivityText(bindingWith(std::string(R"({"integration_id":"ha_main","device_id":"dev_lg_tv",)"
                                                           R"("command_id":"call_service","params":{},)") +
                                               call + "}")
                                       .c_str(),
                                   ActivityShape::Create, &a)
                    .ok());
    ASSERT_TRUE(a.bindings.at(0).actions.at(0).call);
    EXPECT_EQ(a.bindings[0].actions[0].call->data, R"({"button":"INFO"})");

    // call_service needs it; any other command refuses it.
    EXPECT_TRUE(decodeActivityText(bindingWith(R"({"integration_id":"ha_main","device_id":"dev_lg_tv",)"
                                               R"("command_id":"call_service","params":{}})")
                                       .c_str(),
                                   ActivityShape::Create)
                    .has(ErrorCode::Required, "/bindings/info:tap/0/call"));
    EXPECT_TRUE(decodeActivityText(bindingWith(std::string(R"({"integration_id":"ha_main","device_id":"dev_lg_tv",)"
                                                           R"("command_id":"media_player.turn_on","params":{},)") +
                                               call + "}")
                                       .c_str(),
                                   ActivityShape::Create)
                    .has(ErrorCode::UnknownField, "/bindings/info:tap/0/call"));
    // A startup step's path, and the deepest data an activity request may carry.
    const std::string deep = R"({"name":"TV","startup":[{"integration_id":"ha_main","device_id":"dev_lg_tv",)"
                             R"("command_id":"call_service","params":{},"call":{"service":"a.b","data":)"
                             R"({"entity_id":1,"x":{"y":[{}]}}}}],"shutdown":[],"bindings":{"info:tap":[)"
                             R"({"integration_id":"ha_main","device_id":"dev_lg_tv","command_id":"call_service",)"
                             R"("params":{},"call":{"service":"a.b","data":{"x":{"y":[{}]}}}}]}})";
    EXPECT_TRUE(decodeActivityText(deep.c_str(), ActivityShape::Create)
                    .has(ErrorCode::ForbiddenParam, "/startup/0/call/data/entity_id"));
}

TEST(ConfigJson, ACallRoundTripsThroughTheConfigAndABackup) {
    ConfigSpec c = sim::fixtureConfig();
    ActionSpec info = sim::haAction("dev_lg_tv", "call_service");
    info.call = HaServiceCallSpec{"webostv.button", R"({"button":"INFO","n":[1,2.5,true,null]})"};
    c.activities[0].bindings.push_back(sim::bind("info:tap", {info}));
    const std::string text = serializeConfig(c);
    ConfigSpec parsed;
    ASSERT_EQ(parseConfig(text, &parsed).code, ErrorCode::Ok);
    const ActionSpec &back = parsed.activities[0].bindings.back().actions.at(0);
    ASSERT_TRUE(back.call);
    EXPECT_EQ(*back.call, *info.call);
    EXPECT_EQ(serializeConfig(parsed), text);  // stable
    EXPECT_NE(text.find(R"("call":{"service":"webostv.button","data":{"button":"INFO","n":[1,2.5,true,null]}})"),
              std::string::npos);

    ConfigSpec restored;
    ASSERT_EQ(parseBackup(serializeBackup(c, {}), &restored).code, ErrorCode::Ok);
    EXPECT_EQ(*restored.activities[0].bindings.back().actions.at(0).call, *info.call);
}
