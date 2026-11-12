// Device profiles on the hub: the built-in set, parsing a profile document
// against the spec, devices that carry their profile, and adding one.
#include <gtest/gtest.h>

#include <string>

#include "fakes.h"
#include "fixtures.h"
#include "hubbo/config/json.h"
#include "hubbo/config/profile.h"
#include "hubbo/config/service.h"
#include "hubbo/storage/storage.h"

using namespace hubbo;
using namespace hubbo::config;

namespace {

// A minimal valid profile; `extra` is spliced in before the closing brace.
std::string profileJson(const std::string &commands = R"({"Home":{"page":"consumer","usage":"0x0040"}})",
                        const std::string &extra = "") {
    return R"({"schema_version":1,"id":"hubbo:test","revision":2,"source":"hubbo","manufacturer":"Acme",)"
           R"("model":"Box","kind":"streamer","platform":"ble_hid","commands":)" +
           commands + extra + "}";
}

ValidationReport parse(const std::string &json, ProfileDocument *out = nullptr) {
    ProfileDocument doc;
    ValidationReport report;
    parseProfile(json, out != nullptr ? out : &doc, &report);
    return report;
}

std::string encode(const DeviceSpec &d, DeviceDetail detail = DeviceDetail::Stored) {
    JsonDocument doc;
    encodeDevice(d, doc.to<JsonObject>(), detail);
    std::string s;
    serializeJson(doc, s);
    return s;
}

}  // namespace

TEST(BuiltinProfiles, LoadFromTheEmbeddedFiles) {
    ASSERT_EQ(builtinProfilesStatus(), ErrorCode::Ok);
    const ProfileLibrary &lib = builtinProfiles();
    ASSERT_EQ(lib.profiles().size(), 4u);
    EXPECT_EQ(lib.profiles()[0].document.profile.id, "hubbo:android-tv");  // index.json order
    const BuiltinProfile *apple = lib.find("hubbo:apple-tv");
    ASSERT_NE(apple, nullptr);
    EXPECT_EQ(apple->document.platform, "ble_hid");
    EXPECT_EQ(apple->document.kind, "streamer");
    EXPECT_EQ(apple->document.profile.sha256.size(), 64u);
    EXPECT_EQ(apple->document.profile.model, "Apple TV");
    ASSERT_NE(apple->document.profile.findCommand("ControlCenter"), nullptr);
    EXPECT_EQ(*apple->document.profile.findCommand("ControlCenter")->holdMs, 1000u);
    EXPECT_EQ(lib.find("hubbo:nope"), nullptr);
    EXPECT_GE(lib.kinds().size(), 30u);
    EXPECT_TRUE(lib.hasKind("tv"));
    EXPECT_TRUE(lib.hasKind("other"));
    EXPECT_FALSE(lib.hasKind("fridge"));
    EXPECT_NE(lib.buttons().find("\"buttons\""), std::string::npos);
}

// Adding a device copies the profile; it survives the stored shape exactly.
TEST(BuiltinProfiles, EveryOneRoundTripsAsADevice) {
    for (const BuiltinProfile &p : builtinProfiles().profiles()) {
        DeviceSpec d = deviceFromProfile(p);
        EXPECT_EQ(d.name, p.document.profile.model);
        EXPECT_EQ(d.kind, p.document.kind);
        EXPECT_EQ(d.integrationId, "ble");
        EXPECT_EQ(d.provider.type, "ble_hid");
        d.id = "dev_x";
        d.revision = 1;
        const std::string once = encode(d);
        JsonDocument doc;
        ASSERT_EQ(parseJson(once, doc, kMaxConfigFileBytes, kFileNestingLimit), ErrorCode::Ok);
        DeviceSpec back;
        ValidationReport r;
        ASSERT_TRUE(decodeDevice(doc.as<JsonVariantConst>(), DeviceShape::Stored, &back, &r))
            << p.document.profile.id << " " << (r.ok() ? "" : r.issues()[0].path);
        EXPECT_EQ(encode(back), once) << p.document.profile.id;
    }
    const DeviceSpec named = deviceFromProfile(*builtinProfiles().find("hubbo:fire-tv"), "Bedroom", "tv");
    EXPECT_EQ(named.name, "Bedroom");
    EXPECT_EQ(named.kind, "tv");
}

TEST(DeviceProfileJson, TheSpecsShape) {
    DeviceSpec d = deviceFromProfile(*builtinProfiles().find("hubbo:apple-tv"), "Apple TV");
    d.id = "dev_apple_tv";
    d.revision = 3;
    JsonDocument doc;
    encodeDevice(d, doc.to<JsonObject>());
    EXPECT_EQ(doc["profile"]["id"], "hubbo:apple-tv");
    EXPECT_EQ(doc["profile"]["revision"], builtinProfiles().find("hubbo:apple-tv")->document.profile.revision);
    EXPECT_EQ(doc["manufacturer"], "Apple");
    EXPECT_EQ(doc["commands"]["Back"]["page"], "consumer");
    EXPECT_EQ(doc["commands"]["Back"]["usage"], "0x0223");
    EXPECT_EQ(doc["commands"]["ControlCenter"]["hold_ms"], 1000);
    EXPECT_EQ(doc["buttons"]["menu"], "Home");
    // No power steps: the Apple TV sleeps and wakes by itself.
    EXPECT_EQ(doc["power"]["type"], "none");
    EXPECT_EQ(doc["power"].size(), 1u);
    EXPECT_EQ(doc["timing"]["key_delay_ms"], 100);
    EXPECT_TRUE(doc["platform"].isNull());  // the API adds it
    EXPECT_TRUE(doc["meta"].isNull());
    // The list's summary: identity, no commands.
    JsonDocument summary;
    encodeDevice(d, summary.to<JsonObject>(), DeviceDetail::Summary);
    EXPECT_EQ(summary["model"], "Apple TV");
    EXPECT_EQ(summary["profile"]["id"], "hubbo:apple-tv");
    for (const char *key : {"commands", "buttons", "power", "timing", "inputs", "channels"}) {
        EXPECT_TRUE(summary[key].isNull()) << key;
    }
}

TEST(DeviceProfileJson, OnlyBluetoothDevicesCarryAProfile) {
    JsonDocument doc;
    ASSERT_EQ(parseJson(R"({"name":"TV","integration_id":"ha_main","kind":"tv","manufacturer":"LG",
                            "provider":{"type":"home_assistant","entity_id":"media_player.tv"}})",
                        doc, kMaxRequestBytes, kRequestNestingLimit),
              ErrorCode::Ok);
    DeviceSpec d;
    ValidationReport r;
    EXPECT_FALSE(decodeDevice(doc.as<JsonVariantConst>(), DeviceShape::Register, &d, &r));
    EXPECT_TRUE(r.has(ErrorCode::UnknownField, "/manufacturer"));

    ASSERT_EQ(parseJson(R"({"name":"Box","integration_id":"ble","kind":"streamer","provider":{"type":"ble_hid"}})",
                        doc, kMaxRequestBytes, kRequestNestingLimit),
              ErrorCode::Ok);
    r = {};
    EXPECT_FALSE(decodeDevice(doc.as<JsonVariantConst>(), DeviceShape::Register, &d, &r));
    for (const char *field : {"/profile", "/manufacturer", "/model", "/commands"}) {
        EXPECT_TRUE(r.has(ErrorCode::Required, field)) << field;
    }
}

TEST(ParseProfile, AcceptsTheSpec) {
    ProfileDocument doc;
    const ValidationReport r =
        parse(profileJson(R"({"Home":{"page":"consumer","usage":"0x0040","label":"Home"},)"
                          R"("Select":{"page":"keyboard","usage":"0x58"},"0":{"page":"keyboard","usage":"0x27"},)"
                          R"("1":{"page":"keyboard","usage":"0x1E"},"2":{"page":"keyboard","usage":"0x1F"},)"
                          R"("3":{"page":"keyboard","usage":"0x20"},"4":{"page":"keyboard","usage":"0x21"},)"
                          R"("5":{"page":"keyboard","usage":"0x22"},"6":{"page":"keyboard","usage":"0x23"},)"
                          R"("7":{"page":"keyboard","usage":"0x24"},"8":{"page":"keyboard","usage":"0x25"},)"
                          R"("9":{"page":"keyboard","usage":"0x26"}})",
                          R"(,"buttons":{"menu":"Home","info":null},)"
                          R"("power":{"type":"toggle","toggle":[{"command":"Home","hold_ms":2000}]},)"
                          R"("timing":{"key_delay_ms":0,"input_delay_ms":500},)"
                          R"("inputs":[{"name":"HDMI 1","steps":[{"command":"Select"},{"delay_ms":100}]}],)"
                          R"("channels":{"digits":3,"finish":"Select"},"meta":{"anything":[1,2]})"),
              &doc);
    ASSERT_TRUE(r.ok()) << r.issues()[0].path << " " << errorCodeName(r.issues()[0].code);
    EXPECT_EQ(doc.profile.id, "hubbo:test");
    EXPECT_EQ(doc.profile.revision, 2u);
    EXPECT_EQ(doc.profile.commands[1].page, HidPage::Keyboard);
    EXPECT_EQ(doc.profile.commands[1].usage, 0x58);
    ASSERT_EQ(doc.profile.buttons.size(), 2u);
    EXPECT_FALSE(doc.profile.buttons[1].command.has_value());
    EXPECT_EQ(*doc.profile.timing->keyDelayMs, 0u);
    EXPECT_EQ(doc.profile.inputs[0].steps[1].delayMs, 100u);
}

TEST(ParseProfile, RefusesWhatTheSpecDoesnt) {
    struct Case {
        std::string json;
        ErrorCode code;
        const char *path;
    } cases[] = {
        {profileJson(R"({"Home":{"page":"consumer","usage":"0x40"}})"), ErrorCode::WrongType, "/commands/Home/usage"},
        {profileJson(R"({"Home":{"page":"consumer","usage":"0x0400"}})"), ErrorCode::ParamOutOfRange,
         "/commands/Home/usage"},
        {profileJson(R"({"Home":{"page":"keyboard","usage":"0x66"}})"), ErrorCode::ParamOutOfRange,
         "/commands/Home/usage"},
        {profileJson(R"({"Home":{"page":"consumer","usage":"0x00cd"}})"), ErrorCode::WrongType,
         "/commands/Home/usage"},
        {profileJson(R"({"Home":{"page":"mouse","usage":"0x0040"}})"), ErrorCode::WrongType, "/commands/Home/page"},
        {profileJson(R"({"Home":{"page":"consumer","usage":"0x0040"},"home":{"page":"consumer","usage":"0x0040"}})"),
         ErrorCode::DuplicateId, "/commands/home"},
        {profileJson(R"({"Play/Pause":{"page":"consumer","usage":"0x00CD"}})"), ErrorCode::InvalidCommandId,
         "/commands/Play~1Pause"},
        {profileJson(R"({})"), ErrorCode::Required, "/commands"},
        {profileJson(R"({"Home":{"page":"consumer","usage":"0x0040","pronto":"0000"}})"), ErrorCode::UnknownField,
         "/commands/Home/pronto"},
        {profileJson(R"({"Home":{"page":"consumer","usage":"0x0040","hold_ms":20000}})"), ErrorCode::ParamOutOfRange,
         "/commands/Home/hold_ms"},
        {profileJson(R"({"Home":{"page":"consumer","usage":"0x0040"}})", R"(,"buttons":{"menu":"Back"})"),
         ErrorCode::UnknownCommand, "/buttons/menu"},
        {profileJson(R"({"Home":{"page":"consumer","usage":"0x0040"}})", R"(,"buttons":{"nope":"Home"})"),
         ErrorCode::InvalidTrigger, "/buttons/nope"},
        {profileJson(R"({"Home":{"page":"consumer","usage":"0x0040"}})",
                     R"(,"power":{"type":"discrete","on":[{"command":"Back"}],"off":[{"delay_ms":5}]})"),
         ErrorCode::UnknownCommand, "/power/on/0/command"},
        {profileJson(R"({"Home":{"page":"consumer","usage":"0x0040"}})", R"(,"power":{"type":"discrete","on":[]})"),
         ErrorCode::Required, "/power/off"},
        {profileJson(R"({"Home":{"page":"consumer","usage":"0x0040"}})",
                     R"(,"power":{"type":"toggle","toggle":[{"command":"Home","delay_ms":5}]})"),
         ErrorCode::UnknownField, "/power/toggle/0/delay_ms"},
        {profileJson(R"({"Home":{"page":"consumer","usage":"0x0040"}})", R"(,"timing":{"ir_repeats":1})"),
         ErrorCode::UnknownField, "/timing/ir_repeats"},
        {profileJson(R"({"Home":{"page":"consumer","usage":"0x0040"}})", R"(,"channels":{"digits":2})"),
         ErrorCode::UnknownCommand, "/channels/0"},
        {profileJson(R"({"Home":{"page":"consumer","usage":"0x0040"}})", R"(,"color":"red")"), ErrorCode::UnknownField,
         "/color"},
    };
    for (const Case &c : cases) {
        const ValidationReport r = parse(c.json);
        EXPECT_TRUE(r.has(c.code, c.path)) << c.json << "\n  first: "
                                           << (r.ok() ? "ok" : r.issues()[0].path + " " +
                                                                   errorCodeName(r.issues()[0].code));
    }
    std::string ir = profileJson();
    ir.replace(ir.find("\"ble_hid\""), 9, "\"ir\"");
    EXPECT_TRUE(parse(ir).has(ErrorCode::UnsupportedSchema, "/platform"));
    std::string v2 = profileJson();
    v2.replace(v2.find("\"schema_version\":1"), 18, "\"schema_version\":2");
    EXPECT_TRUE(parse(v2).has(ErrorCode::UnsupportedSchema, "/schema_version"));
    std::string source = profileJson();
    source.replace(source.find("\"source\":\"hubbo\""), 16, "\"source\":\"user\"");
    EXPECT_TRUE(parse(source).has(ErrorCode::InvalidId, "/source"));
    ProfileDocument doc;
    EXPECT_EQ(parseProfile("{", &doc, nullptr), ErrorCode::MalformedJson);
}

TEST(ProfileLibrary, ChecksTheIndex) {
    const std::string kinds = R"({"schema_version":1,"kinds":[{"id":"streamer","label":"Streamer","harmony":[35]}]})";
    const std::string index = R"([{"file":"t.json","id":"hubbo:test","revision":2,"sha256":")" +
                              std::string(64, 'a') + R"("}])";
    const std::string profile = profileJson();
    ProfileFile files[] = {{"kinds.json", kinds.c_str()},
                           {"buttons.json", "{}"},
                           {"index.json", index.c_str()},
                           {"t.json", profile.c_str()}};
    ProfileLibrary lib;
    ASSERT_EQ(lib.load(files, 4), ErrorCode::Ok);
    EXPECT_EQ(lib.find("hubbo:test")->document.profile.sha256, std::string(64, 'a'));

    const std::string stale = R"([{"file":"t.json","id":"hubbo:test","revision":1,"sha256":")" +
                              std::string(64, 'a') + R"("}])";
    files[2].text = stale.c_str();
    ValidationReport r;
    EXPECT_EQ(lib.load(files, 4, &r), ErrorCode::ValidationFailed);
    EXPECT_TRUE(r.has(ErrorCode::RevisionConflict, "/index.json/0/revision"));
    EXPECT_TRUE(lib.profiles().empty());

    const std::string noKind = R"({"schema_version":1,"kinds":[{"id":"tv","label":"TV"}]})";
    files[2].text = index.c_str();
    files[0].text = noKind.c_str();
    r = {};
    EXPECT_EQ(lib.load(files, 4, &r), ErrorCode::ValidationFailed);
    EXPECT_TRUE(r.has(ErrorCode::UnknownKind, "/hubbo:test/kind"));
}

// Adding a device from a profile through the one pipeline; kinds come from
// kinds.json.
TEST(AddFromProfile, ThroughConfigService) {
    storage::MemoryStorage mem;
    storage::SlotRecordStore records(mem, kMaxConfigFileBytes);
    sim::FakeAdapters fakes;
    Engine::Store store;
    ConfigService service(records, fakes.set, store);
    ASSERT_EQ(service.load(sim::fixtureConfig()).state, ConfigState::Fresh);

    const Outcome added = service.createDevice(deviceFromProfile(*builtinProfiles().find("hubbo:fire-tv")));
    ASSERT_TRUE(added.ok()) << (added.issues.ok() ? errorCodeName(added.code) : added.issues.issues()[0].path);
    const DeviceSpec *d = service.findDevice(added.id);
    ASSERT_NE(d, nullptr);
    ASSERT_TRUE(d->profile.has_value());
    EXPECT_EQ(d->profile->id, "hubbo:fire-tv");

    const Outcome badKind =
        service.createDevice(deviceFromProfile(*builtinProfiles().find("hubbo:apple-tv"), "", "fridge"));
    EXPECT_EQ(badKind.code, ErrorCode::ValidationFailed);
    EXPECT_TRUE(badKind.issues.has(ErrorCode::UnknownKind, "/kind"))
        << (badKind.issues.ok() ? "" : badKind.issues.issues()[0].path);
}
