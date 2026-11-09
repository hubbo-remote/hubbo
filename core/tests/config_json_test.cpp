// Config <-> JSON: round trips, strict shape errors with JSON Pointer paths,
// bounded parsing and schema versions.
#include <gtest/gtest.h>

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

    ASSERT_EQ(parseJson(R"({"name":"TV","integration_id":"ha_main","kind":"fridge",
                            "provider":{"type":"zigbee"}})",
                        doc, kMaxRequestBytes, kRequestNestingLimit),
              ErrorCode::Ok);
    r = {};
    EXPECT_FALSE(decodeDevice(doc.as<JsonVariantConst>(), DeviceShape::Register, &d, &r));
    EXPECT_TRUE(r.has(ErrorCode::WrongType, "/kind"));
    EXPECT_TRUE(r.has(ErrorCode::WrongType, "/provider/type"));

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
    encodeDevice(d, response.to<JsonObject>(), /*withCapabilities=*/false);
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

TEST(ConfigJson, ARetiredCaCertLoadsAndIsDroppedOnSave) {
    // The board's saved config (and backups made before) carry ca_cert_pem:
    // they must still load, or the hub boots Blocked after the update.
    ConfigSpec c = sim::fixtureConfig();
    std::string text = serializeConfig(c);
    const std::string from = R"("verify_tls":true)";
    const size_t at = text.find(from);
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, from.size(), R"("verify_tls":false,"ca_cert_pem":"-----BEGIN CERTIFICATE-----")");
    ConfigSpec loaded;
    const ParseResult r = parseConfig(text, &loaded);
    ASSERT_EQ(r.code, ErrorCode::Ok) << r.report.issues().size();
    const IntegrationSpec *ha = nullptr;
    for (const IntegrationSpec &i : loaded.integrations) {
        if (i.id == "ha_main") ha = &i;
    }
    ASSERT_NE(ha, nullptr);
    EXPECT_FALSE(ha->verifyTls);  // kept
    EXPECT_EQ(serializeConfig(loaded).find("ca_cert_pem"), std::string::npos);

    // A backup too (null, as older backups wrote it).
    std::string backup = serializeBackup(c, BackupInfo());
    const size_t b = backup.find(from);
    ASSERT_NE(b, std::string::npos) << backup;
    backup.replace(b, from.size(), R"("verify_tls":true,"ca_cert_pem":null)");
    ConfigSpec restored;
    EXPECT_EQ(parseBackup(backup, &restored).code, ErrorCode::Ok);
    EXPECT_EQ(serializeBackup(restored, BackupInfo()).find("ca_cert_pem"), std::string::npos);
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
