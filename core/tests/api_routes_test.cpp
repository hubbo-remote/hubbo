// The /api/v1 handlers end to end, in process: real router, policy,
// authenticator, credentials, ConfigService and engine (SimHub with fake
// adapters). api/contract.sh does the same over HTTP and validates every
// body against api/openapi.yaml; these tests pin behaviour in C++.
#include <gtest/gtest.h>

#include <ArduinoJson.h>

#include <cstdlib>
#include <map>
#include <string>

#include "auth_test_util.h"
#include "firmware_image_util.h"
#include "fake_ha.h"
#include "fake_mqtt.h"
#include "fixtures.h"
#include "hub.h"
#include "hubbo/api/agent_docs.h"
#include "hubbo/api/api.h"
#include "hubbo/api/transitions.h"
#include "hubbo/config/profile.h"
#include "hubbo/config/service.h"
#include "hubbo/integrations/ha_client.h"
#include "hubbo/integrations/mqtt_bridge.h"

using namespace hubbo;

namespace {

// Runtime over a SimHub: engine requests run by advancing simulated time.
// Follows the api::Runtime::run contract: false only if submit() fails;
// once submitted, wait for done() however long it takes (the engine's own
// timeouts bound it), never return with the request still in flight.
struct SimRuntime : api::Runtime {
    explicit SimRuntime(sim::SimHub &h) : hub(h) {}
    bool run(EngineRequest &request) override {
        if (!hub.engine().submit(request)) return false;
        // The engine bounds every request; a minute of simulated time
        // without an answer is an engine bug, so fail loudly, don't hang.
        for (uint32_t waited = 0; !request.done(); waited++) {
            if (waited > 60000) std::abort();
            hub.advance(1);
        }
        return true;
    }
    api::RuntimeSnapshot snapshot() const override {
        api::RuntimeSnapshot s;
        s.uptimeMs = hub.nowMs();
        if (hub.engine().hasActiveActivity()) s.activeActivityId = hub.engine().activeActivityId();
        s.remote.paired = true;
        return s;
    }
    const AdapterSet &adapters() const override { return hub.adapters(); }
    const char *integrationState(std::string_view) const override { return "connected"; }
    sim::SimHub &hub;
};

struct Reply;
std::string dump(const Reply &r);

struct Reply {
    int status = 0;
    JsonDocument json;
    std::map<std::string, std::string> headers;
    std::string contentType;
    std::string body;  // raw
    std::string code() const { return json["error"]["code"] | ""; }
};

std::string dump(const Reply &r) {
    std::string out;
    serializeJson(r.json, out);
    return out;
}

class ApiTest : public ::testing::Test {
protected:
    ApiTest() {
        seeded = hub.load(sim::fixtureConfig());
        creds.load();
        api.registerRoutes(router);
    }

    void SetUp() override {
        ASSERT_TRUE(seeded.ok()) << "sim::fixtureConfig() no longer loads: "
                                 << (seeded.issues().empty() ? std::string()
                                                             : std::string(errorCodeName(seeded.issues()[0].code)) +
                                                                   " at " + seeded.issues()[0].path);
    }

    Reply call(http::Method method, const std::string &path, const std::string &body = "",
               std::map<std::string, std::string> extra = {}) {
        auth::test::Headers headers;
        headers.items = {{"host", "127.0.0.1"}};
        if (!body.empty()) headers.items["content-type"] = "application/json";
        if (!token.empty()) headers.items["authorization"] = "Bearer " + token;
        if (!cookie.empty()) headers.items["cookie"] = "hubbo_session=" + cookie;
        for (auto &h : extra) headers.items[h.first] = h.second;
        http::Request r;
        r.method = method;
        const size_t q = path.find('?');
        const std::string fullPath = path.substr(0, q);
        const std::string query = q == std::string::npos ? "" : path.substr(q + 1);
        r.path = fullPath;
        r.query = query;
        r.body = body;
        r.headers = &headers;
        r.context = noAuth ? simulatorContext() : authn.authenticate(r);
        http::Response out;
        router.dispatch(r, out);
        Reply reply;
        reply.status = out.status;
        reply.contentType = out.contentType;
        reply.body = out.body;
        if (!out.body.empty()) deserializeJson(reply.json, out.body);
        for (auto &h : out.headers) reply.headers[h.first] = h.second;
        return reply;
    }

    static auth::RequestContext simulatorContext() {
        auth::RequestContext c;
#ifdef HUBBO_SIMULATOR_NO_AUTH
        c.principal = auth::Principal::simulator();
#else
        c.principal = auth::Principal::admin();
#endif
        return c;
    }

    void loginAsAdmin() {
        noAuth = false;
        ASSERT_EQ(call(http::Method::Post, "/api/v1/auth/setup",
                       R"({"username":"admin","password":"correct horse battery"})").status,
                  201);
        Reply r = call(http::Method::Post, "/api/v1/auth/login",
                       R"({"username":"admin","password":"correct horse battery"})");
        ASSERT_EQ(r.status, 200);
        const std::string set = r.headers["Set-Cookie"];
        cookie = set.substr(set.find('=') + 1, set.find(';') - set.find('=') - 1);
    }

    // The integration task's loop, against the fake HA.
    void pumpHa(int steps = 5) {
        for (int i = 0; i < steps; i++) {
            haNow += 100;
            haTransport.poll(0);
            haClient.poll(haNow);
            haTransport.poll(0);
            while (haClient.hasWork()) {
                haClient.poll(haNow);
                haTransport.poll(0);
            }
        }
    }
    // Stores the fake HA's token and connects, as the platform does at boot.
    void connectHa() {
        ASSERT_TRUE(secrets.write(api::kHaTokenKey, haServer.token));
        api::applyHomeAssistantSettings(hub.configService(), &secrets, haClient);
        pumpHa();
        ASSERT_EQ(haClient.state(), integrations::HaState::Connected);
    }
    // A finished discovery (GET's reply).
    Reply discover(const std::string &query = "") {
        Reply r = call(http::Method::Post, "/api/v1/admin/integrations/ha_main/discover");
        EXPECT_EQ(r.status, 202) << dump(r);
        pumpHa();
        return call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover" + query);
    }

    // An entity's loaded details (GET's reply), as registering it needs.
    Reply details(const std::string &entityId) {
        const std::string path = "/api/v1/admin/integrations/ha_main/entities/" + entityId + "/details";
        Reply r = call(http::Method::Post, path);
        EXPECT_EQ(r.status, 202) << dump(r);
        pumpHa();
        return call(http::Method::Get, path);
    }

    sim::SimHub hub;
    ValidationReport seeded;
    uint64_t haNow = 0;
    SimRuntime runtime{hub};
    // Erasing `failEraseKey` fails (one secret of several).
    struct Secrets : auth::MemorySecretStore {
        std::string failEraseKey;
        bool erase(std::string_view key) override {
            return key != failEraseKey && auth::MemorySecretStore::erase(key);
        }
    } secrets;
    auth::SoftwareHash hash;
    auth::test::CountingRandom random;
    auth::Credentials creds{secrets, hash, random, auth::kMinPasswordIterations};
    auth::SessionStore sessions{random};
    auth::test::FakeClock clock;
    api::Authenticator authn{creds, sessions, clock};
    // Phase 3 ports: the remote radio and a real HA client against the fake HA.
    api::SimulatedRemote remote;
    sim::SimulatedBleHosts &ble = hub.bleHosts();
    sim::FakeHaServer haServer;
    sim::FakeHaTransport haTransport{haServer};
    integrations::HomeAssistantClient haClient{haTransport};
    api::SimulatedFirmwareUpdate firmware;
    api::SimulatedFactoryReset factoryReset;
    storage::MemoryStateStore hubState;
    config::HubSettings hubSettings{hubState, "Hubbo 534D"};
    api::SimulatedRestart restart;
    sim::FakeMqttBroker broker;
    sim::FakeMqttTransport mqttTransport{broker};
    integrations::MqttBridge mqtt{mqttTransport, "hubbo_02000000534d",
                                  {[] { return 0u; }, [] { return std::string(); },
                                   [] { return integrations::MqttDeviceInfo{}; }, [](const std::string &) {}, [] {}}};
    api::Api api{api::ApiContext{creds, sessions, clock, hub.configService(), runtime, {}, &remote,
                                 &haClient, &secrets, nullptr, &ble, &firmware, &factoryReset, &hubSettings,
                                 &restart, &mqtt}};
    http::Router router;

    bool noAuth = true;  // simulated admin unless a test logs in
    std::string token, cookie;
};

const char *kActivity = R"({"name":"Movie Night","startup":[
  {"integration_id":"ha_main","device_id":"dev_lg_tv","command_id":"media_player.turn_on","params":{}}],
  "shutdown":[{"integration_id":"ha_main","device_id":"dev_lg_tv","command_id":"media_player.turn_off","params":{}}],
  "bindings":{"volume_up:down":[{"integration_id":"ha_main","device_id":"dev_denon",
  "command_id":"media_player.volume_up","params":{}}]}})";

}  // namespace

TEST_F(ApiTest, ActivityCrudActivateOff) {
    Reply r = call(http::Method::Post, "/api/v1/activities", kActivity);
    ASSERT_EQ(r.status, 201) << r.json.as<std::string>();
    const std::string id = r.json["id"].as<std::string>();
    // One generation counter: a revision is the config generation of the
    // object's last change.
    const uint32_t created = r.json["revision"];
    EXPECT_EQ(created, hub.configService().generation());
    EXPECT_EQ(r.headers["Location"], "/api/v1/activities/" + id);

    r = call(http::Method::Get, "/api/v1/activities/" + id);
    ASSERT_EQ(r.status, 200);
    r.json["name"] = "Movie Night 2";
    std::string body;
    serializeJson(r.json, body);
    Reply replaced = call(http::Method::Put, "/api/v1/activities/" + id, body);
    ASSERT_EQ(replaced.status, 200);
    const uint32_t current = replaced.json["revision"];
    EXPECT_GT(current, created);
    Reply stale = call(http::Method::Put, "/api/v1/activities/" + id, body);
    EXPECT_EQ(stale.status, 409);
    EXPECT_EQ(stale.code(), "revision_conflict");
    EXPECT_EQ(stale.json["error"]["current_revision"], current);

    r = call(http::Method::Post, "/api/v1/activities/" + id + "/activate");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.json["switched"], true);
    EXPECT_EQ(r.json["active_activity_id"], id) << dump(r);
    EXPECT_EQ(r.json["startup"]["status"], "queued");
    EXPECT_EQ(r.json["shutdown"]["status"], "none");
    EXPECT_EQ(call(http::Method::Get, "/api/v1/activities").json["active_activity_id"], id);
    r = call(http::Method::Post, "/api/v1/activities/off");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.json["switched"], true);
    EXPECT_TRUE(r.json["active_activity_id"].isNull());
    EXPECT_EQ(r.json["previous_activity_id"], id);
    EXPECT_EQ(r.json["shutdown"]["status"], "queued");
    EXPECT_EQ(r.json["shutdown"]["queued_actions"], 1);
    r = call(http::Method::Post, "/api/v1/activities/off");
    EXPECT_EQ(r.status, 200) << "off with nothing active is a no-op";
    EXPECT_EQ(r.json["switched"], false);
    EXPECT_EQ(r.json["shutdown"]["status"], "none");

    EXPECT_EQ(call(http::Method::Delete, "/api/v1/activities/" + id).status, 422);
    EXPECT_EQ(call(http::Method::Delete, "/api/v1/activities/" + id + "?revision=" + std::to_string(created)).status,
              409);
    EXPECT_EQ(call(http::Method::Delete, "/api/v1/activities/" + id + "?revision=" + std::to_string(current)).status,
              204);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/activities/" + id).status, 404);
    EXPECT_EQ(call(http::Method::Post, "/api/v1/activities/act_nope/activate").status, 404);
}

TEST_F(ApiTest, ValidationAndRequestLimits) {
    Reply r = call(http::Method::Post, "/api/v1/activities/validate", kActivity);
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.json["valid"], true) << r.json.as<std::string>() << dump(r);

    std::string bad = kActivity;
    bad.replace(bad.find("\"params\":{}"), 11, R"("params":{"entity_id":"all"})");
    r = call(http::Method::Post, "/api/v1/activities/validate", bad);
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.json["valid"], false);
    r = call(http::Method::Post, "/api/v1/activities", bad);
    EXPECT_EQ(r.status, 422);
    EXPECT_EQ(r.code(), "validation_failed");
    EXPECT_EQ(r.json["error"]["details"][0]["path"], "/startup/0/params/entity_id");

    EXPECT_EQ(call(http::Method::Post, "/api/v1/activities", "{oops").status, 400);
    EXPECT_EQ(call(http::Method::Post, "/api/v1/activities", std::string(20, '[') + std::string(20, ']')).status,
              400);
    EXPECT_EQ(call(http::Method::Post, "/api/v1/activities", kActivity, {{"content-type", "text/plain"}}).status,
              415);
    EXPECT_EQ(call(http::Method::Post, "/api/v1/activities", std::string(40000, ' ')).status, 413);
    r = call(http::Method::Post, "/api/v1/activities", kActivity, {{"origin", "http://evil.example"}});
    EXPECT_EQ(r.status, 403);
    EXPECT_EQ(r.code(), "csrf_failed");
}

TEST_F(ApiTest, DevicesCapabilitiesExecute) {
    Reply r = call(http::Method::Get, "/api/v1/devices");
    ASSERT_EQ(r.status, 200);
    EXPECT_GT(r.json["devices"].size(), 0u);
    r = call(http::Method::Get, "/api/v1/devices/dev_denon/capabilities");
    ASSERT_EQ(r.status, 200);
    bool hasVolumeSet = false;
    for (JsonVariant c : r.json["commands"].as<JsonArray>()) {
        hasVolumeSet = hasVolumeSet || c["command_id"] == "media_player.volume_set";
    }
    EXPECT_TRUE(hasVolumeSet);

    const std::string exec = "/api/v1/devices/dev_denon/commands/media_player.volume_set/execute";
    r = call(http::Method::Post, exec, R"({"params":{"level":0.35}})");
    ASSERT_EQ(r.status, 200) << r.code();
    EXPECT_EQ(r.json["status"], "accepted");
    EXPECT_EQ(call(http::Method::Post, exec, R"({"params":{"level":1.5}})").status, 422);
    EXPECT_EQ(call(http::Method::Post, exec, R"({"params":{"level":0.2,"entity_id":"all"}})").status, 422);
    EXPECT_EQ(call(http::Method::Post, exec, R"({"params":{"level":{"target":"all"}}})").status, 422);
    EXPECT_EQ(call(http::Method::Post, exec, R"({})").status, 422);
    // Holds are for bindings and waits for sequences: execute refuses both.
    const std::string apple = "/api/v1/devices/dev_apple_tv/commands/";
    r = call(http::Method::Post, apple + "press_down/execute", R"({"params":{"command":"Select"}})");
    EXPECT_EQ(r.status, 422);
    EXPECT_EQ(r.json["error"]["details"][0]["code"], "hold_not_allowed");
    r = call(http::Method::Post, apple + "wait_connected/execute", R"({"params":{}})");
    EXPECT_EQ(r.status, 422) << dump(r);
    EXPECT_EQ(r.json["error"]["details"][0]["code"], "sequence_only");
    EXPECT_EQ(r.json["error"]["details"][0]["path"], "/command_id");
    r = call(http::Method::Post, apple + "press/execute", R"({"params":{"command":"Select"}})");
    EXPECT_EQ(r.status, 200) << dump(r);
    r = call(http::Method::Post, apple + "press/execute", R"({"params":{"command":"Home","hold_ms":2000}})");
    EXPECT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(call(http::Method::Post, apple + "press/execute", R"({"params":{"command":"Netflix"}})").status, 422);
    r = call(http::Method::Post, apple + "usage_tap/execute", R"({"params":{"usage":205}})");
    EXPECT_EQ(r.status, 200) << dump(r);
    r = call(http::Method::Get, "/api/v1/devices/dev_apple_tv/capabilities");
    for (JsonVariant c : r.json["commands"].as<JsonArray>()) {
        const std::string id = c["command_id"];
        const bool down = id == "key_down" || id == "press_down" || id == "usage_down";
        EXPECT_EQ(c["effect"], id == "wait_connected" ? "wait" : down ? "hold" : "momentary") << id;
        const char *group = id.rfind("press", 0) == 0 ? "remote"
                            : id.rfind("key_", 0) == 0 ? "keys"
                            : id.rfind("usage_", 0) == 0 ? "custom"
                                                         : nullptr;
        if (group == nullptr) {
            EXPECT_TRUE(c["group"].isNull()) << id;
        } else {
            EXPECT_EQ(c["group"], group) << id;
        }
        if (id.rfind("press", 0) == 0) {
            EXPECT_EQ(c["params"][0]["label"], "Remote command") << id;
        }
    }
    r = call(http::Method::Get, "/api/v1/devices/dev_denon/capabilities");
    for (JsonVariant c : r.json["commands"].as<JsonArray>()) {
        EXPECT_TRUE(c["group"].isNull());
        for (JsonVariant p : c["params"].as<JsonArray>()) EXPECT_TRUE(p["label"].isNull());
    }

    // The list is light; the detail carries the profile; HA devices have none of it.
    r = call(http::Method::Get, "/api/v1/devices");
    for (JsonVariant d : r.json["devices"].as<JsonArray>()) {
        EXPECT_EQ(d["platform"], d["provider"]["type"]);
        EXPECT_TRUE(d["commands"].isNull());
        if (d["id"] == "dev_apple_tv") {
            EXPECT_EQ(d["profile"]["id"], "hubbo:apple-tv");
            EXPECT_EQ(d["model"], "Apple TV");
        }
    }
    r = call(http::Method::Get, "/api/v1/devices/dev_apple_tv");
    EXPECT_EQ(r.json["commands"]["Select"]["usage"], "0x0041");
    EXPECT_EQ(r.json["power"]["type"], "none");
    r = call(http::Method::Get, "/api/v1/devices/dev_denon");
    EXPECT_TRUE(r.json["commands"].isNull());
    EXPECT_TRUE(r.json["profile"].isNull());
    EXPECT_EQ(r.json["platform"], "home_assistant");
    EXPECT_EQ(call(http::Method::Post, "/api/v1/devices/dev_nope/commands/media_player.turn_on/execute",
                   R"({"params":{}})").status,
              404);
}

TEST_F(ApiTest, AdminDeviceRegistry) {
    sim::FakeHaEntity backlight;
    backlight.info.entityId = "switch.tv_backlight";
    backlight.uniqueId = "uid_backlight";
    haServer.addEntity(backlight);
    // Registration uses the entity's loaded details: none yet.
    Reply r = call(http::Method::Post, "/api/v1/admin/devices",
                   R"({"name":"TV Backlight","integration_id":"ha_main","kind":"switch",
                       "provider":{"type":"home_assistant","entity_id":"switch.tv_backlight"}})");
    EXPECT_EQ(r.status, 409) << dump(r);
    EXPECT_EQ(r.code(), "details_required");
    connectHa();
    r = details("switch.tv_backlight");
    ASSERT_EQ(r.json["state"], "done") << dump(r);
    EXPECT_EQ(r.json["provider"]["unique_id"], "uid_backlight");
    EXPECT_EQ(r.json["supported"], true);
    EXPECT_TRUE(r.json["skipped_services"].is<JsonArray>());
    // Another entity's details are idle; registering with them is refused.
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/integrations/ha_main/entities/light.family_room/details")
                  .json["state"],
              "idle");
    r = call(http::Method::Post, "/api/v1/admin/devices",
                   R"({"name":"TV Backlight","integration_id":"ha_main","kind":"switch",
                       "provider":{"type":"home_assistant","entity_id":"switch.tv_backlight"}})");
    ASSERT_EQ(r.status, 201) << dump(r);
    EXPECT_FALSE(r.json["ha_capabilities"].is<JsonObject>()) << "stored, not part of Device";
    EXPECT_EQ(r.json["provider"]["unique_id"], "uid_backlight");  // filled from the details
    const std::string id = r.json["id"].as<std::string>();
    const uint32_t created = r.json["revision"];
    const DeviceSpec *stored = hub.configService().findDevice(id);
    ASSERT_NE(stored, nullptr);
    ASSERT_TRUE(stored->haCapabilities.has_value());
    EXPECT_EQ(stored->haCapabilities->platform, "demo");
    // The hub reads capabilities; a client can't supply them.
    r = call(http::Method::Post, "/api/v1/admin/devices",
             R"({"name":"Fake","integration_id":"ha_main","ha_capabilities":{},
                 "provider":{"type":"home_assistant","entity_id":"switch.tv_backlight"}})");
    EXPECT_EQ(r.status, 422);
    EXPECT_EQ(r.json["error"]["details"][0]["code"], "unknown_field");
    // One device per entity, also after HA renames it: the details find the
    // device by unique_id (within the platform) and registering it again is refused.
    ASSERT_EQ(details("switch.tv_backlight").json["registered_device_id"], id);
    r = call(http::Method::Post, "/api/v1/admin/devices",
             R"({"name":"Again","integration_id":"ha_main",
                 "provider":{"type":"home_assistant","entity_id":"switch.tv_backlight"}})");
    EXPECT_EQ(r.status, 422);
    EXPECT_EQ(r.json["error"]["details"][0]["code"], "duplicate_id");
    haServer.removeEntity("switch.tv_backlight");
    sim::FakeHaEntity renamed = backlight;
    renamed.info.entityId = "switch.tv_glow";
    haServer.addEntity(renamed);
    r = details("switch.tv_glow");
    ASSERT_EQ(r.json["state"], "done") << dump(r);
    EXPECT_EQ(r.json["registered_device_id"], id) << "a renamed entity is still the registered device";
    r = call(http::Method::Post, "/api/v1/admin/devices",
             R"({"name":"Glow","integration_id":"ha_main",
                 "provider":{"type":"home_assistant","entity_id":"switch.tv_glow"}})");
    EXPECT_EQ(r.json["error"]["details"][0]["code"], "duplicate_id") << dump(r);
    // Re-linking the device itself to the new entity_id is the way.
    r = call(http::Method::Put, "/api/v1/admin/devices/" + id,
             R"({"provider":{"type":"home_assistant","entity_id":"switch.tv_glow"},"revision":)" +
                 std::to_string(hub.configService().findDevice(id)->revision) + "}");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["provider"]["entity_id"], "switch.tv_glow");

    r = details("switch.not_in_ha");
    EXPECT_EQ(r.json["state"], "failed");
    EXPECT_EQ(r.json["error"], "switch.not_in_ha isn't in Home Assistant");
    r = call(http::Method::Post, "/api/v1/admin/devices",
             R"({"name":"Ghost","integration_id":"ha_main",
                 "provider":{"type":"home_assistant","entity_id":"switch.not_in_ha"}})");
    EXPECT_EQ(r.code(), "details_required");
    // Bad details requests.
    const std::string base = "/api/v1/admin/integrations/";
    EXPECT_EQ(call(http::Method::Post, base + "ha_main/entities/sensor.temp/details").status, 422);
    EXPECT_EQ(call(http::Method::Post, base + "ha_main/entities/light.all/details").status, 422);
    EXPECT_EQ(call(http::Method::Post, base + "ble/entities/light.office/details").status, 404);
    r = call(http::Method::Put, "/api/v1/admin/devices/" + id,
             R"({"name":"Backlight","revision":)" +
                 std::to_string(hub.configService().findDevice(id)->revision) + "}");
    ASSERT_EQ(r.status, 200) << dump(r);
    const uint32_t updated = r.json["revision"];
    EXPECT_GT(updated, created);
    EXPECT_EQ(call(http::Method::Delete, "/api/v1/admin/devices/" + id + "?revision=" + std::to_string(created)).status,
              409);
    EXPECT_EQ(call(http::Method::Delete, "/api/v1/admin/devices/" + id + "?revision=" + std::to_string(updated)).status,
              204);

    r = call(http::Method::Post, "/api/v1/admin/devices",
             R"({"name":"Everything","integration_id":"ha_main",
                 "provider":{"type":"home_assistant","entity_id":"light.all"}})");
    EXPECT_EQ(r.status, 422);

    const uint32_t rev = hub.configService().findDevice("dev_denon")->revision;
    r = call(http::Method::Delete, "/api/v1/admin/devices/dev_denon?revision=" + std::to_string(rev));
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(r.code(), "device_in_use");
    EXPECT_GT(r.json["error"]["details"].size(), 0u);
}

TEST_F(ApiTest, AuthFlowAndRoleMatrix) {
    noAuth = false;
    Reply s = call(http::Method::Get, "/api/v1/auth/session");
    EXPECT_EQ(s.json["setup_required"], true);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/devices").status, 401);
    EXPECT_EQ(call(http::Method::Post, "/api/v1/auth/login", R"({"username":"admin","password":"x"})").status, 409);

    loginAsAdmin();
    s = call(http::Method::Get, "/api/v1/auth/session");
    EXPECT_EQ(s.json["role"], "admin");
    EXPECT_TRUE(s.json["csrf_token"].isNull()) << "no CSRF token any more";
    EXPECT_EQ(s.json["expires_in_s"], auth::kSessionLifetimeMs / 1000);
    Reply r = call(http::Method::Post, "/api/v1/auth/setup", R"({"username":"x","password":"another password"})");
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(r.code(), "setup_already_done");
    EXPECT_EQ(call(http::Method::Post, "/api/v1/auth/setup", "{oops").status, 409) << "decided before the body";

    // A cookie session needs no CSRF header; a cross-site Origin is refused.
    r = call(http::Method::Post, "/api/v1/admin/tokens", R"({"name":"agent"})", {{"origin", "http://evil.example"}});
    EXPECT_EQ(r.status, 403);
    EXPECT_EQ(r.code(), "csrf_failed");
    r = call(http::Method::Post, "/api/v1/admin/tokens", R"({"name":"agent"})");
    ASSERT_EQ(r.status, 201);
    const std::string plaintext = r.json["token"].as<std::string>();
    const std::string tokenId = r.json["id"].as<std::string>();
    r = call(http::Method::Get, "/api/v1/admin/tokens");
    EXPECT_TRUE(r.json["tokens"][0]["token"].isNull()) << "never listed again";

    // Token: allowed reads/writes, 403 on admin.
    const std::string adminCookie = cookie;
    cookie.clear();
    token = plaintext;
    EXPECT_EQ(call(http::Method::Get, "/api/v1/devices").status, 200);
    EXPECT_EQ(call(http::Method::Post, "/api/v1/activities", kActivity).status, 201);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/tokens").status, 403);
    EXPECT_EQ(call(http::Method::Post, "/api/v1/admin/devices", "{}").status, 403);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/backup").status, 403);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/firmware").status, 403);
    EXPECT_EQ(call(http::Method::Post, "/api/v1/admin/firmware/update", hubbo::test::image(),
                   {{"content-type", "application/octet-stream"}, {"content-length", "1"}})
                  .status,
              403);
    EXPECT_EQ(firmware.status().state, api::FirmwareUpdateState::Idle) << "a token can't update firmware";
    EXPECT_EQ(call(http::Method::Post, "/api/v1/admin/factory-reset").status, 403);
    EXPECT_FALSE(factoryReset.pending()) << "a token can't factory reset";
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/hub").status, 403);
    EXPECT_EQ(call(http::Method::Put, "/api/v1/admin/hub", R"({"name":"Den"})").status, 403);
    EXPECT_EQ(hubSettings.identity().name, "Hubbo 534D") << "a token can't rename the hub";
    EXPECT_EQ(call(http::Method::Post, "/api/v1/admin/restart").status, 403);
    EXPECT_FALSE(restart.pending()) << "a token can't restart the hub";

    // Revoke and logout.
    token.clear();
    cookie = adminCookie;
    EXPECT_EQ(call(http::Method::Delete, "/api/v1/admin/tokens/" + tokenId).status, 204);
    cookie.clear();
    token = plaintext;
    EXPECT_EQ(call(http::Method::Get, "/api/v1/devices").status, 401);
    token.clear();
    cookie = adminCookie;
    r = call(http::Method::Post, "/api/v1/auth/logout");
    EXPECT_EQ(r.status, 204);
    EXPECT_NE(r.headers["Set-Cookie"].find("Max-Age=0"), std::string::npos);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/devices").status, 401);
}

TEST_F(ApiTest, WrongPasswordsAndPasswordChange) {
    loginAsAdmin();
    const std::string adminCookie = cookie;
    cookie.clear();
    Reply r;
    for (int i = 0; i < 10; i++) {  // no rate limit: the hash's cost slows guessing
        r = call(http::Method::Post, "/api/v1/auth/login", R"({"username":"admin","password":"wrong password!"})");
        EXPECT_EQ(r.status, 401);
        EXPECT_EQ(r.code(), "invalid_credentials");
    }
    EXPECT_EQ(call(http::Method::Post, "/api/v1/auth/login",
                   R"({"username":"admin","password":"correct horse battery"})")
                  .status,
              200);

    cookie = adminCookie;
    r = call(http::Method::Put, "/api/v1/admin/account/password",
             R"({"current_password":"correct horse battery","new_password":"short"})");
    EXPECT_EQ(r.status, 422);
    EXPECT_EQ(r.json["error"]["details"][0]["code"], "weak_password");
    r = call(http::Method::Put, "/api/v1/admin/account/password",
             R"({"current_password":"correct horse battery","new_password":"a much better password"})");
    EXPECT_EQ(r.status, 204);
    EXPECT_TRUE(creds.verifyLogin("admin", "a much better password"));
}

TEST_F(ApiTest, BackupAndRestore) {
    Reply backup = call(http::Method::Get, "/api/v1/admin/backup");
    ASSERT_EQ(backup.status, 200);
    EXPECT_EQ(backup.json["format"], "hubbo-backup");
    std::string archive;
    serializeJson(backup.json, archive);
    // No secret-bearing keys (the `excluded` list naming them is fine).
    EXPECT_EQ(archive.find("\"password\""), std::string::npos);
    EXPECT_EQ(archive.find("\"access_token\""), std::string::npos);
    EXPECT_EQ(archive.find("pbkdf2"), std::string::npos);
    const size_t activities = hub.config().activities.size();

    // Change something, then restore the archive: the change is undone.
    ASSERT_EQ(call(http::Method::Post, "/api/v1/activities", kActivity).status, 201);
    Reply dry = call(http::Method::Post, "/api/v1/admin/restore?dry_run=true", archive);
    ASSERT_EQ(dry.status, 200) << dump(dry);
    EXPECT_EQ(dry.json["dry_run"], true);
    EXPECT_EQ(hub.config().activities.size(), activities + 1) << "a dry run changes nothing";
    Reply done = call(http::Method::Post, "/api/v1/admin/restore", archive);
    ASSERT_EQ(done.status, 200) << dump(done);
    EXPECT_EQ(done.json["activities"], activities);
    EXPECT_EQ(hub.config().activities.size(), activities);

    EXPECT_EQ(call(http::Method::Post, "/api/v1/admin/restore", "{nope").status, 400);
    EXPECT_EQ(call(http::Method::Post, "/api/v1/admin/restore", R"({"format":"something-else"})").status, 422);
}

namespace {

// The running config as a saved record, with schema_version replaced.
std::string recordWithSchema(const ConfigSpec &config, int schemaVersion) {
    JsonDocument doc;
    deserializeJson(doc, config::serializeConfig(config));
    doc["schema_version"] = schemaVersion;
    std::string out;
    serializeJson(doc, out);
    return out;
}

}  // namespace

TEST_F(ApiTest, BackupWhileBlockedExportsTheSavedRecord) {
    const ConfigSpec good = hub.config();
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/backup").headers["X-Hubbo-Backup"], "config");

    // A config from newer firmware: Blocked, nothing runs.
    const std::string newer = recordWithSchema(good, 99);
    uint64_t next = 0;
    ASSERT_EQ(hub.records().nextGeneration("config", &next), ErrorCode::Ok);
    ASSERT_EQ(hub.records().save("config", next, newer), ErrorCode::Ok);
    hub.configService().load(good);
    ASSERT_EQ(hub.configService().state(), config::ConfigState::Blocked);
    ASSERT_TRUE(hub.config().activities.empty());

    // The backup is the saved bytes, marked, never the empty runtime config.
    Reply raw = call(http::Method::Get, "/api/v1/admin/backup");
    ASSERT_EQ(raw.status, 200);
    EXPECT_EQ(raw.contentType, "application/octet-stream");
    EXPECT_EQ(raw.headers["X-Hubbo-Backup"], "blocked-raw");
    EXPECT_NE(raw.headers["Content-Disposition"].find("hubbo-backup-BLOCKED-"), std::string::npos)
        << raw.headers["Content-Disposition"];
    EXPECT_EQ(raw.body, newer);

    // This firmware can't read it: refused, still Blocked, slots untouched.
    Reply refused = call(http::Method::Post, "/api/v1/admin/restore", raw.body);
    ASSERT_EQ(refused.status, 422) << dump(refused);
    EXPECT_EQ(refused.json["error"]["details"][0]["code"], "unsupported_schema") << dump(refused);
    EXPECT_EQ(refused.json["error"]["details"][0]["path"], "/schema_version");
    EXPECT_EQ(hub.configService().state(), config::ConfigState::Blocked);
    storage::Record saved;
    ASSERT_EQ(hub.records().load("config", &saved), ErrorCode::Ok);
    EXPECT_EQ(saved.payload, newer);

    // A record it can read (an export from a hub blocked for another
    // reason, or after a firmware update) restores and unblocks.
    const std::string readable = recordWithSchema(good, config::kConfigFileVersion);
    Reply dry = call(http::Method::Post, "/api/v1/admin/restore?dry_run=true", readable);
    ASSERT_EQ(dry.status, 200) << dump(dry);
    EXPECT_EQ(hub.configService().state(), config::ConfigState::Blocked) << "a dry run changes nothing";
    Reply restored = call(http::Method::Post, "/api/v1/admin/restore", readable);
    ASSERT_EQ(restored.status, 200) << dump(restored);
    EXPECT_EQ(restored.json["activities"], good.activities.size());
    EXPECT_EQ(hub.configService().state(), config::ConfigState::Ok);
    EXPECT_EQ(hub.config().activities.size(), good.activities.size());
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/backup").headers["X-Hubbo-Backup"], "config");

    // A record that parses but isn't a config this firmware accepts.
    EXPECT_EQ(call(http::Method::Post, "/api/v1/admin/restore", R"({"format":"hubbo-config","schema_version":"x"})")
                  .status,
              422);
}

TEST_F(ApiTest, BackupRefusesWhenItCantReadTheRecord) {
    const ConfigSpec good = hub.config();
    // Both config slots damaged: Blocked (Corrupt), and there's nothing
    // intact to export.
    auto &medium = static_cast<storage::MemoryStorage &>(hub.storage());
    uint64_t next = 0;
    ASSERT_EQ(hub.records().nextGeneration("config", &next), ErrorCode::Ok);
    ASSERT_EQ(hub.records().save("config", next, config::serializeConfig(good)), ErrorCode::Ok);
    for (const char *slot : {"config.a", "config.b"}) {
        std::string &bytes = medium.files()[slot];
        ASSERT_FALSE(bytes.empty()) << slot;
        bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0x5A);
    }
    hub.configService().load(good);
    ASSERT_EQ(hub.configService().state(), config::ConfigState::Blocked);
    Reply corrupt = call(http::Method::Get, "/api/v1/admin/backup");
    EXPECT_EQ(corrupt.status, 409) << dump(corrupt);
    EXPECT_EQ(corrupt.code(), "config_blocked");
    EXPECT_EQ(corrupt.headers.count("X-Hubbo-Backup"), 0u);

    // The load itself couldn't read storage: nothing trustworthy runs, so
    // 503, never an empty backup. (Reset first: Blocked stays Blocked.)
    Reply reset = call(http::Method::Post, "/api/v1/admin/reset", "");
    ASSERT_EQ(reset.status, 200) << dump(reset);
    medium.setHealth(storage::Health::Unavailable);
    hub.configService().load(good);
    ASSERT_EQ(hub.configService().state(), config::ConfigState::StorageUnavailable);
    ASSERT_EQ(hub.configService().lastLoad().state, config::ConfigState::StorageUnavailable);
    Reply down = call(http::Method::Get, "/api/v1/admin/backup");
    EXPECT_EQ(down.status, 503) << dump(down);
    EXPECT_EQ(down.code(), "storage_unavailable");
    medium.setHealth(storage::Health::Available);
    hub.configService().refresh(0);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/backup").status, 200);
}

TEST_F(ApiTest, BackupWorksWhenStorageFailsAfterAGoodLoad) {
    // Loaded fine, then the medium is distrusted (ReadOnly after a failed
    // invalidation) or goes away: the running config is the last
    // acknowledged save, and the admin must be able to export it.
    auto &medium = static_cast<storage::MemoryStorage &>(hub.storage());
    Reply before = call(http::Method::Get, "/api/v1/admin/backup");
    ASSERT_EQ(before.status, 200);
    for (const storage::Health health : {storage::Health::ReadOnly, storage::Health::Unavailable}) {
        medium.setHealth(health);
        ASSERT_EQ(hub.configService().state(), config::ConfigState::StorageUnavailable);
        Reply backup = call(http::Method::Get, "/api/v1/admin/backup");
        ASSERT_EQ(backup.status, 200) << dump(backup);
        EXPECT_EQ(backup.headers["X-Hubbo-Backup"], "config");
        EXPECT_EQ(backup.json["activities"], before.json["activities"]);
        EXPECT_EQ(backup.json["devices"], before.json["devices"]);
        EXPECT_EQ(backup.json["activities"].size(), hub.config().activities.size());
        // /status says what's wrong now, not the stale reason of the load.
        Reply status = call(http::Method::Get, "/api/v1/status");
        EXPECT_EQ(status.json["config"]["state"], "storage_unavailable");
        EXPECT_NE(std::string(status.json["config"]["reason"] | ""), "ok");
        EXPECT_NE(std::string(status.json["config"]["reason"] | "").find("unavailable"), std::string::npos)
            << dump(status);
    }
    medium.setHealth(storage::Health::Available);
}

TEST_F(ApiTest, Status) {
    Reply r = call(http::Method::Get, "/api/v1/status");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.json["auth_mode"], "enforced");
    EXPECT_GT(r.json["integrations"].size(), 0u);
    r = call(http::Method::Get, "/api/v1/remote/buttons");
    EXPECT_EQ(r.json["buttons"].size(), remote::kButtonCount);
}

TEST_F(ApiTest, FirmwareUpdate) {
    using hubbo::test::image;
    using hubbo::test::ImageSpec;
    auto upload = [&](const std::string &bytes, const char *type = "application/octet-stream") {
        return call(http::Method::Post, "/api/v1/admin/firmware/update", bytes,
                    {{"content-type", type}, {"content-length", std::to_string(bytes.size())}});
    };
    Reply r = call(http::Method::Get, "/api/v1/admin/firmware");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["update"]["state"], "idle");
    EXPECT_EQ(r.json["update"]["progress"], 0);
    EXPECT_TRUE(r.json["update"]["error"].isNull());

    // Each refusal changes nothing.
    EXPECT_EQ(upload(image(), "application/json").status, 415);
    r = call(http::Method::Post, "/api/v1/admin/firmware/update", image(),
             {{"content-type", "application/octet-stream"}});
    EXPECT_EQ(r.status, 411);
    EXPECT_EQ(r.code(), "length_required");
    ImageSpec big;
    big.padTo = api::simulatorFirmwareTarget().slotBytes;
    EXPECT_EQ(upload(image(big)).code(), "payload_too_large");
    ImageSpec s;
    s.descMagic = 0;  // a merged factory .bin
    r = upload(image(s));
    EXPECT_EQ(r.status, 422);
    EXPECT_EQ(r.code(), "invalid_image");
    s = {};
    s.marker = "HUBBO-IMAGE/1 board=waveshare_esp32_s3_eth layout=1 version=v9";
    EXPECT_EQ(upload(image(s)).code(), "wrong_board");
    s = {};
    s.badHash = true;
    EXPECT_EQ(upload(image(s)).code(), "invalid_image");
    EXPECT_EQ(upload("").code(), "invalid_image");
    EXPECT_EQ(firmware.status().state, api::FirmwareUpdateState::Idle);

    r = upload(image());
    ASSERT_EQ(r.status, 202) << dump(r);
    EXPECT_EQ(r.json["update"]["state"], "writing");
    r = upload(image());
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(r.code(), "update_in_progress");
    firmware.advance(api::SimulatedFirmwareUpdate::kWriteMs / 4);
    r = call(http::Method::Get, "/api/v1/admin/firmware");
    EXPECT_EQ(r.json["update"]["progress"], 25);
    firmware.advance(api::SimulatedFirmwareUpdate::kWriteMs);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/firmware").json["update"]["state"], "rebooting");

    firmware.advance(api::SimulatedFirmwareUpdate::kRebootMs);
    firmware.failWrites = true;
    ASSERT_EQ(upload(image()).status, 202);
    firmware.advance(api::SimulatedFirmwareUpdate::kWriteMs);
    r = call(http::Method::Get, "/api/v1/admin/firmware");
    EXPECT_EQ(r.json["update"]["state"], "failed");
    EXPECT_EQ(r.json["update"]["error"], "write_failed");
    firmware.ready = false;
    EXPECT_EQ(upload(image()).status, 503);
}

// The calls MQTT shares with the routes (api/transitions.h).
TEST_F(ApiTest, SharedTransitionAndRestart) {
    api::TransitionOutcome t = api::runTransition(hub.configService(), runtime, "act_play_xbox");
    ASSERT_EQ(t.status, api::TransitionStatus::Done);
    EXPECT_TRUE(t.result.switched);
    EXPECT_EQ(t.result.activeActivityId.view(), "act_play_xbox");
    EXPECT_EQ(call(http::Method::Get, "/api/v1/activities").json["active_activity_id"], "act_play_xbox");

    t = api::runTransition(hub.configService(), runtime, "act_missing");
    ASSERT_EQ(t.status, api::TransitionStatus::Done);
    EXPECT_FALSE(t.result.switched);
    EXPECT_EQ(t.result.error, ErrorCode::UnknownActivity);

    t = api::runTransition(hub.configService(), runtime, "");  // off
    ASSERT_EQ(t.status, api::TransitionStatus::Done);
    EXPECT_TRUE(t.result.switched);
    EXPECT_EQ(t.result.previousActivityId.view(), "act_play_xbox");
    t = api::runTransition(hub.configService(), runtime, "");  // nothing active: a no-op
    ASSERT_EQ(t.status, api::TransitionStatus::Done);
    EXPECT_FALSE(t.result.switched);
    EXPECT_EQ(t.result.error, ErrorCode::NoActiveActivity);

    // The update and refusal rules go through POST /admin/restart (HubNameAndRestart).
    EXPECT_EQ(api::requestHubRestart(nullptr, &firmware), api::RestartOutcome::NoPort);
    EXPECT_EQ(api::requestHubRestart(&restart, nullptr), api::RestartOutcome::Accepted);  // no firmware port
    EXPECT_TRUE(restart.pending());
}

TEST_F(ApiTest, HubNameAndRestart) {
    Reply r = call(http::Method::Get, "/api/v1/admin/hub");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["name"], "Hubbo 534D");  // the platform's default
    EXPECT_EQ(r.json["hostname"], "hubbo-534d");
    EXPECT_EQ(r.json["hostname_conflict"], false);

    // A clash on the network: both routes show what mDNS really advertises.
    hubSettings.setAdvertisedHostname("hubbo-534d-2");
    r = call(http::Method::Get, "/api/v1/admin/hub");
    EXPECT_EQ(r.json["hostname"], "hubbo-534d-2");
    EXPECT_EQ(r.json["hostname_conflict"], true);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/status").json["network"]["hostname"], "hubbo-534d-2");

    std::vector<config::HubIdentity> applied;
    hubSettings.onChanged([&](const config::HubIdentity &identity) { applied.push_back(identity); });
    r = call(http::Method::Put, "/api/v1/admin/hub", R"({"name":"  Living Room  "})");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["name"], "Living Room");  // trimmed
    EXPECT_EQ(r.json["hostname"], "living-room");
    EXPECT_EQ(r.json["hostname_conflict"], false) << "a rename clears the advertised hostname";
    ASSERT_EQ(applied.size(), 1u);
    EXPECT_EQ(applied[0].hostname, "living-room");
    std::string saved;
    EXPECT_EQ(hubState.get(config::kHubNameKey, &saved), ErrorCode::Ok);
    EXPECT_EQ(saved, "Living Room");
    r = call(http::Method::Put, "/api/v1/admin/hub", R"({"name":"Living Room"})");  // unchanged
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["hostname"], "living-room");
    EXPECT_EQ(applied.size(), 1u);  // nothing re-applied
    r = call(http::Method::Get, "/api/v1/status");
    EXPECT_EQ(r.json["hubbo"]["name"], "Living Room");
    EXPECT_EQ(r.json["network"]["hostname"], "living-room");  // live

    // Invalid: 422 at /name, nothing changes.
    for (const char *body : {R"({"name":"   "})", R"({"name":"a\u0007b"})",
                             R"({"name":"123456789012345678901234567890123"})"}) {
        r = call(http::Method::Put, "/api/v1/admin/hub", body);
        EXPECT_EQ(r.status, 422) << body;
        EXPECT_EQ(r.code(), "validation_failed") << body;
        EXPECT_EQ(r.json["error"]["details"][0]["path"], "/name") << body;
    }
    for (const char *body : {R"({"name":7})", R"({"name":"Den","color":"red"})", R"({})"}) {
        EXPECT_EQ(call(http::Method::Put, "/api/v1/admin/hub", body).status, 422) << body;
    }
    EXPECT_EQ(call(http::Method::Put, "/api/v1/admin/hub", "{").status, 400);
    hubState.failWrites = true;
    EXPECT_EQ(call(http::Method::Put, "/api/v1/admin/hub", R"({"name":"Den"})").status, 503);
    hubState.failWrites = false;
    EXPECT_EQ(hubSettings.identity().name, "Living Room");
    EXPECT_EQ(applied.size(), 1u);

    // Restart: refused during a firmware update and when the platform can't.
    ASSERT_EQ(call(http::Method::Post, "/api/v1/admin/firmware/update", hubbo::test::image(),
                   {{"content-type", "application/octet-stream"},
                    {"content-length", std::to_string(hubbo::test::image().size())}})
                  .status,
              202);
    r = call(http::Method::Post, "/api/v1/admin/restart");
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(r.code(), "update_in_progress");
    firmware.advance(api::SimulatedFirmwareUpdate::kWriteMs + api::SimulatedFirmwareUpdate::kRebootMs);
    restart.ready = false;
    EXPECT_EQ(call(http::Method::Post, "/api/v1/admin/restart").status, 503);
    restart.ready = true;
    r = call(http::Method::Post, "/api/v1/admin/restart");
    ASSERT_EQ(r.status, 202) << dump(r);
    EXPECT_EQ(r.body, "{}");
    EXPECT_EQ(call(http::Method::Post, "/api/v1/admin/restart").status, 202);  // again while pending
    restart.advance(api::SimulatedRestart::kRestartMs);
    EXPECT_EQ(restart.restarts(), 1);
}

// Without HubSettings the name routes answer 503 and /status still names
// the hub (hubbo.name is required): the default.
TEST_F(ApiTest, StatusNamesAHubWithoutSettings) {
    api::Api bare{api::ApiContext{creds, sessions, clock, hub.configService(), runtime, {}, nullptr,
                                  nullptr, nullptr, nullptr}};
    http::Router bareRouter;
    bare.registerRoutes(bareRouter);
    std::swap(router, bareRouter);
    Reply r = call(http::Method::Get, "/api/v1/status");
    std::swap(router, bareRouter);
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["hubbo"]["name"], "Hubbo");
    EXPECT_TRUE(r.json["network"]["hostname"].isNull());
}

TEST_F(ApiTest, AgentDocsArePublicAndErrorsPointToThem) {
    noAuth = false;  // no admin set up, no credentials

    for (const char *path : {"/api/v1", "/api/v1/"}) {
        Reply r = call(http::Method::Get, path);
        ASSERT_EQ(r.status, 200) << path;
        EXPECT_EQ(r.json["name"], "Hubbo API");
        EXPECT_EQ(r.json["version"], "0.0.0");
        EXPECT_EQ(r.json.size(), 6u);  // name, version, about, docs, openapi, auth
        EXPECT_NE(r.json["about"].as<std::string>().find("agent guide"), std::string::npos);
        EXPECT_EQ(r.json["docs"], "/api/v1/agent.md");
        EXPECT_EQ(r.json["openapi"], "/api/v1/openapi.json");
        EXPECT_NE(r.json["auth"].as<std::string>().find("Bearer"), std::string::npos);
        EXPECT_TRUE(r.json["hubbo"].isNull());
    }

    // The spec: JSON (plain curl), gzip when asked, 304 on a current ETag.
    Reply spec = call(http::Method::Get, "/api/v1/openapi.json");
    ASSERT_EQ(spec.status, 200);
    EXPECT_EQ(spec.contentType, "application/json");
    EXPECT_EQ(spec.headers.count("Content-Encoding"), 0u);
    EXPECT_EQ(spec.headers["Vary"], "Accept-Encoding");
    JsonDocument parsed;
    ASSERT_EQ(deserializeJson(parsed, spec.body, DeserializationOption::NestingLimit(64)), DeserializationError::Ok);
    EXPECT_EQ(parsed["openapi"], "3.1.0");
    EXPECT_FALSE(parsed["paths"]["/agent.md"].isNull());
    Reply packed = call(http::Method::Get, "/api/v1/openapi.json", "", {{"accept-encoding", "gzip, deflate, br"}});
    ASSERT_EQ(packed.status, 200);
    EXPECT_EQ(packed.headers["Content-Encoding"], "gzip");
    ASSERT_GT(packed.body.size(), 2u);
    EXPECT_EQ(packed.body.substr(0, 2), "\x1f\x8b");  // gzip's magic
    EXPECT_LT(packed.body.size(), spec.body.size());
    const std::string etag = spec.headers["ETag"];
    ASSERT_EQ(etag.size(), 18u) << etag;  // quoted
    EXPECT_EQ(packed.headers["ETag"], etag);  // one version, either encoding
    Reply cached = call(http::Method::Get, "/api/v1/openapi.json", "", {{"if-none-match", etag}});
    EXPECT_EQ(cached.status, 304);
    EXPECT_TRUE(cached.body.empty());
    EXPECT_EQ(call(http::Method::Get, "/api/v1/openapi.json", "", {{"if-none-match", "\"old\""}}).status, 200);

    Reply guide = call(http::Method::Get, "/api/v1/agent.md");
    ASSERT_EQ(guide.status, 200);
    EXPECT_EQ(guide.contentType, "text/markdown; charset=utf-8");
    EXPECT_EQ(guide.body.rfind("# ", 0), 0u);
    EXPECT_NE(guide.headers["ETag"], etag);

    Reply llms = call(http::Method::Get, "/llms.txt");
    ASSERT_EQ(llms.status, 200);
    EXPECT_EQ(llms.contentType, "text/markdown; charset=utf-8");
    EXPECT_EQ(llms.body, api::docs::kLlmsTxt);
    EXPECT_NE(llms.body.find("](/api/v1/agent.md)"), std::string::npos);
    EXPECT_NE(llms.body.find("](/api/v1/openapi.json)"), std::string::npos);
    EXPECT_EQ(call(http::Method::Post, "/llms.txt").status, 405);

    // Hints: a 401, and a 404 for an unknown /api path, point to the guide.
    Reply r = call(http::Method::Get, "/api/v1/devices");
    EXPECT_EQ(r.status, 401);
    EXPECT_EQ(r.json["error"]["docs"], "/api/v1/agent.md");
    for (const char *path : {"/api", "/api/", "/api/v2", "/api/v1/nope", "/api/v1/devices/dev_1/nope"}) {
        r = call(http::Method::Get, path);
        EXPECT_EQ(r.status, 404) << path;
        EXPECT_EQ(r.json["error"]["docs"], "/api/v1/agent.md") << path;
    }
    // Not elsewhere: outside the API, a 405, or a missing id on a known route.
    for (const char *path : {"/nope", "/apis", "/"}) {
        r = call(http::Method::Get, path);
        EXPECT_EQ(r.status, 404) << path;
        EXPECT_TRUE(r.json["error"]["docs"].isNull()) << path;
    }
    r = call(http::Method::Delete, "/api/v1/openapi.json");
    EXPECT_EQ(r.status, 405);
    EXPECT_TRUE(r.json["error"]["docs"].isNull());
    noAuth = true;
    r = call(http::Method::Get, "/api/v1/devices/dev_missing");
    EXPECT_EQ(r.status, 404);
    EXPECT_TRUE(r.json["error"]["docs"].isNull());
}

TEST_F(ApiTest, FactoryReset) {
    ASSERT_EQ(creds.setupAdmin("hubbo-owner", "correct horse battery"), auth::CredentialResult::Ok);
    ASSERT_TRUE(secrets.write(api::kHaTokenKey, "ha-secret"));
    std::string sessionId;
    ASSERT_TRUE(sessions.create(clock.mono, &sessionId));
    factoryReset.erase = [&] {  // what hub-sim's restart does with the auth state
        EXPECT_TRUE(auth::eraseAllSecrets(secrets));
        creds.load();
        sessions.clear();
    };

    // Refused during a firmware update, and when the platform can't.
    ASSERT_EQ(call(http::Method::Post, "/api/v1/admin/firmware/update", hubbo::test::image(),
                   {{"content-type", "application/octet-stream"},
                    {"content-length", std::to_string(hubbo::test::image().size())}})
                  .status,
              202);
    Reply r = call(http::Method::Post, "/api/v1/admin/factory-reset");
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(r.code(), "update_in_progress");
    firmware.advance(api::SimulatedFirmwareUpdate::kWriteMs + api::SimulatedFirmwareUpdate::kRebootMs);
    factoryReset.ready = false;
    r = call(http::Method::Post, "/api/v1/admin/factory-reset");
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(r.code(), "unavailable");
    EXPECT_FALSE(factoryReset.pending());

    // 202 first; the erase comes with the simulated restart.
    factoryReset.ready = true;
    r = call(http::Method::Post, "/api/v1/admin/factory-reset");
    ASSERT_EQ(r.status, 202) << dump(r);
    EXPECT_TRUE(factoryReset.pending());
    EXPECT_EQ(creds.state(), auth::CredentialState::Ok);
    factoryReset.advance(api::SimulatedFactoryReset::kRestartMs - 1);
    EXPECT_EQ(creds.state(), auth::CredentialState::Ok);
    factoryReset.advance(1);
    EXPECT_FALSE(factoryReset.pending());
    EXPECT_EQ(creds.state(), auth::CredentialState::Fresh);
    std::string value;
    EXPECT_EQ(secrets.read(api::kHaTokenKey, &value), auth::SecretRead::NotFound);
    EXPECT_FALSE(sessions.authenticate(sessionId, clock.mono).valid);
    EXPECT_EQ(creds.setupGuard(), auth::CredentialResult::Ok) << "setup is open again";
}

TEST_F(ApiTest, RemoteEndpointAndHubScan) {
    Reply r = call(http::Method::Get, "/api/v1/admin/remote");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["radio"], "idle");
    EXPECT_TRUE(r.json["endpoint"].isNull());
    EXPECT_EQ(r.json["pairing"]["state"], "idle");

    // The console's `radio set` rule.
    EXPECT_EQ(call(http::Method::Put, "/api/v1/admin/remote/endpoint", R"({"channel":125,"address":"0x1234567890"})")
                  .status,
              422);
    for (const char *bad : {"0x0000000000", "FFFFFFFFFF", "12345678AA", "12345678xx", "12345678901"}) {
        r = call(http::Method::Put, "/api/v1/admin/remote/endpoint",
                 std::string(R"({"channel":5,"address":")") + bad + "\"}");
        EXPECT_EQ(r.status, 422) << bad;
    }
    r = call(http::Method::Put, "/api/v1/admin/remote/endpoint", R"({"channel":5,"address":"0xab12cd34ef"})");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["radio"], "listening");
    EXPECT_EQ(r.json["endpoint"]["channel"], 5);
    EXPECT_EQ(r.json["endpoint"]["address"], "AB12CD34EF");

    r = call(http::Method::Post, "/api/v1/admin/remote/pair");
    ASSERT_EQ(r.status, 202) << dump(r);
    EXPECT_EQ(r.json["state"], "scanning");
    EXPECT_EQ(r.json["remaining_s"], 120);
    // Busy while scanning.
    r = call(http::Method::Put, "/api/v1/admin/remote/endpoint", R"({"channel":6,"address":"AB12CD34EF"})");
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(r.code(), "busy");
    remote.found({40, 0x1122334466});
    r = call(http::Method::Get, "/api/v1/admin/remote/pair");
    EXPECT_EQ(r.json["state"], "found");
    EXPECT_EQ(r.json["channel"], 40);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/remote").json["endpoint"]["address"], "1122334466");

    call(http::Method::Post, "/api/v1/admin/remote/pair");
    remote.advance(api::kRemoteScanTimeoutMs);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/remote/pair").json["state"], "timeout");
    call(http::Method::Post, "/api/v1/admin/remote/pair");
    EXPECT_EQ(call(http::Method::Delete, "/api/v1/admin/remote/pair").status, 204);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/remote/pair").json["state"], "stopped");

    remote.ready = false;
    EXPECT_EQ(call(http::Method::Post, "/api/v1/admin/remote/pair").status, 503);
}

TEST_F(ApiTest, ProfilesAndKinds) {
    Reply r = call(http::Method::Get, "/api/v1/profiles");
    ASSERT_EQ(r.status, 200) << dump(r);
    ASSERT_EQ(r.json["profiles"].size(), config::builtinProfiles().profiles().size());
    EXPECT_EQ(r.json["profiles"][1]["id"], "hubbo:apple-tv");  // index.json order
    EXPECT_EQ(r.json["profiles"][1]["platform"], "ble_hid");
    EXPECT_EQ(r.json["profiles"][1]["sha256"].as<std::string>().size(), 64u);

    r = call(http::Method::Get, "/api/v1/profiles/hubbo:apple-tv");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["model"], "Apple TV");
    EXPECT_EQ(r.json["commands"]["ControlCenter"]["hold_ms"], 1000);
    EXPECT_TRUE(r.json["meta"].isNull());
    EXPECT_EQ(call(http::Method::Get, "/api/v1/profiles/hubbo%3Aapple-tv").status, 200);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/profiles/hubbo:roku").status, 404);

    r = call(http::Method::Get, "/api/v1/kinds");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.json["kinds"][0]["id"], "tv");
    EXPECT_EQ(r.json["kinds"][0]["label"], "TV");
    EXPECT_TRUE(r.json["kinds"][0]["harmony"].isNull());

    // Adding from a profile: name and kind default from it; the client sends nothing else.
    r = call(http::Method::Post, "/api/v1/admin/devices", R"({"profile_id":"hubbo:fire-tv"})");
    ASSERT_EQ(r.status, 201) << dump(r);
    EXPECT_EQ(r.json["name"], "Fire TV");
    EXPECT_EQ(r.json["kind"], "streamer");
    EXPECT_EQ(r.json["integration_id"], "ble");
    EXPECT_EQ(r.json["platform"], "ble_hid");
    EXPECT_EQ(r.json["manufacturer"], "Amazon");
    EXPECT_EQ(r.json["commands"]["Back"]["page"], "consumer");
    r = call(http::Method::Post, "/api/v1/admin/devices", R"({"profile_id":"hubbo:fire-tv","name":"Den","kind":"set_top_box"})");
    ASSERT_EQ(r.status, 201) << dump(r);
    EXPECT_EQ(r.json["kind"], "set_top_box");

    struct Bad {
        const char *body, *path, *code;
    } bad[] = {
        {R"({"profile_id":"hubbo:roku"})", "/profile_id", "unknown_profile"},
        {R"({"profile_id":"hubbo:fire-tv","kind":"toaster"})", "/kind", "unknown_kind"},
        {R"({"profile_id":"hubbo:fire-tv","commands":{}})", "/commands", "unknown_field"},
        {R"({"profile_id":"hubbo:fire-tv","integration_id":"ble"})", "/integration_id", "unknown_field"},
        {R"({"profile":{"id":"user:x"},"name":"X","integration_id":"ha_main",
             "provider":{"type":"home_assistant","entity_id":"switch.x"}})",
         "/profile", "unknown_field"},
        {R"({"name":"X","integration_id":"ha_main","model":"X",
             "provider":{"type":"home_assistant","entity_id":"switch.x"}})",
         "/model", "unknown_field"},
        {R"({"name":"X","integration_id":"ble","kind":"streamer","provider":{"type":"ble_hid"}})", "/profile_id",
         "required"},
    };
    for (const Bad &b : bad) {
        r = call(http::Method::Post, "/api/v1/admin/devices", b.body);
        EXPECT_EQ(r.status, 422) << b.body;
        EXPECT_EQ(r.json["error"]["details"][0]["path"], b.path) << dump(r);
        EXPECT_EQ(r.json["error"]["details"][0]["code"], b.code) << dump(r);
    }
}

TEST_F(ApiTest, BluetoothPairing) {
    const std::string apple = "/api/v1/admin/devices/dev_apple_tv/pair";
    auto state = [&](const std::string &id) {
        return call(http::Method::Get, "/api/v1/devices/" + id).json["bluetooth"]["state"].as<std::string>();
    };
    Reply r = call(http::Method::Get, apple);
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["state"], "connected");  // the fixture world's bonded Apple TV
    EXPECT_FALSE(call(http::Method::Get, "/api/v1/devices/dev_denon").json["bluetooth"].is<JsonObject>());

    r = call(http::Method::Post, "/api/v1/admin/devices", R"({"profile_id":"hubbo:android-tv","name":"Shield"})");
    ASSERT_EQ(r.status, 201) << dump(r);
    EXPECT_EQ(r.json["bluetooth"]["state"], "not_paired");
    const std::string shield = r.json["id"];
    const std::string pair = "/api/v1/admin/devices/" + shield + "/pair";

    r = call(http::Method::Post, pair);
    ASSERT_EQ(r.status, 202) << dump(r);
    EXPECT_EQ(r.json["state"], "pairing");
    EXPECT_EQ(r.json["window_s"], 120);
    EXPECT_EQ(state(shield), "pairing");
    EXPECT_EQ(call(http::Method::Post, pair).status, 202) << "its own window: same answer";
    r = call(http::Method::Post, apple);
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(r.code(), "busy");

    // A host bonds: connected, then not.
    ASSERT_TRUE(ble.bonded("Shield"));
    EXPECT_EQ(call(http::Method::Get, pair).json["state"], "connected");
    ASSERT_TRUE(ble.setConnected(shield, false));
    EXPECT_EQ(state(shield), "not_connected");
    EXPECT_EQ(state("dev_apple_tv"), "connected") << "only its own host";

    // A re-pair that's cancelled or runs out keeps the old host; forget drops it.
    ASSERT_EQ(call(http::Method::Post, pair + "?window_s=30").json["window_s"], 30);
    EXPECT_EQ(call(http::Method::Delete, pair).status, 204);
    EXPECT_EQ(state(shield), "not_connected");
    ASSERT_EQ(call(http::Method::Post, pair).status, 202);
    ble.advance(120000);
    EXPECT_EQ(state(shield), "not_connected");
    EXPECT_EQ(call(http::Method::Delete, "/api/v1/admin/devices/" + shield + "/pairing").status, 204);
    EXPECT_EQ(state(shield), "not_paired");

    // At most 4 Bluetooth devices.
    std::vector<std::string> added{shield};
    for (const char *name : {"Fire TV", "Office TV"}) {
        r = call(http::Method::Post, "/api/v1/admin/devices",
                 std::string(R"({"profile_id":"hubbo:fire-tv","name":")") + name + "\"}");
        ASSERT_EQ(r.status, 201) << dump(r);
        added.push_back(r.json["id"]);
    }
    r = call(http::Method::Post, "/api/v1/admin/devices", R"({"profile_id":"hubbo:fire-tv","name":"Fifth"})");
    EXPECT_EQ(r.status, 422);
    EXPECT_EQ(r.json["error"]["details"][0]["path"], "/integration_id");
    EXPECT_EQ(r.json["error"]["details"][0]["code"], "too_many_devices");
    EXPECT_EQ(r.json["error"]["message"], "You can add up to 4 Bluetooth devices");

    // Deleting a Bluetooth device forgets its host.
    ASSERT_EQ(call(http::Method::Post, pair).status, 202);
    ASSERT_TRUE(ble.bonded("Shield"));
    for (const std::string &id : added) {
        const uint32_t rev = hub.configService().findDevice(id)->revision;
        ASSERT_EQ(call(http::Method::Delete, "/api/v1/admin/devices/" + id + "?revision=" + std::to_string(rev)).status,
                  204);
    }
    EXPECT_FALSE(ble.status(shield).bonded);

    for (const char *bad : {"29", "301", "x", "1000", "-5"}) {
        r = call(http::Method::Post, apple + "?window_s=" + bad);
        EXPECT_EQ(r.status, 422) << bad;
    }
    r = call(http::Method::Post, "/api/v1/admin/devices/dev_denon/pair");
    EXPECT_EQ(r.status, 404) << "not a Bluetooth device";
    EXPECT_EQ(r.code(), "not_found");
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/devices/nope/pair").status, 404);
    ble.ready = false;
    EXPECT_EQ(call(http::Method::Post, apple).status, 503);
    EXPECT_EQ(call(http::Method::Delete, "/api/v1/admin/devices/dev_apple_tv/pairing").status, 503);
}

TEST_F(ApiTest, TheHomeAssistantTokenIsWriteOnly) {
    const std::string token = haServer.token;
    Reply r = call(http::Method::Get, "/api/v1/admin/integrations/ha_main");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["settings"]["has_access_token"], false);
    uint32_t revision = r.json["revision"];

    r = call(http::Method::Put, "/api/v1/admin/integrations/ha_main",
             R"({"settings":{"type":"home_assistant","access_token":")" + token + R"("},"revision":)" +
                 std::to_string(revision) + "}");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.body.find(token), std::string::npos);
    EXPECT_EQ(r.json["settings"]["has_access_token"], true);
    revision = r.json["revision"];
    std::string stored;
    ASSERT_EQ(secrets.read(api::kHaTokenKey, &stored), auth::SecretRead::Ok);
    EXPECT_EQ(stored, token);
    pumpHa();
    EXPECT_EQ(haClient.state(), integrations::HaState::Connected);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/backup").body.find(token), std::string::npos);

    // Omitted: kept. A new URL without a token: cleared.
    r = call(http::Method::Put, "/api/v1/admin/integrations/ha_main",
             R"({"name":"HA","revision":)" + std::to_string(revision) + "}");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["settings"]["has_access_token"], true);
    revision = r.json["revision"];
    r = call(http::Method::Put, "/api/v1/admin/integrations/ha_main",
             R"({"settings":{"type":"home_assistant","url":"http://other.local:8123"},"revision":)" +
                 std::to_string(revision) + "}");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["settings"]["has_access_token"], false);
    EXPECT_EQ(secrets.read(api::kHaTokenKey, &stored), auth::SecretRead::NotFound);
    pumpHa();
    EXPECT_EQ(haClient.state(), integrations::HaState::NotConfigured);
    revision = r.json["revision"];

    // Bad shapes, a stale revision, another integration's settings.
    for (const char *bad : {R"("url":"http://user@ha.local")", R"("url":"ftp://ha.local")",
                            R"("url":"http://ha.local/path")", R"("access_token":"")", R"("access_token":5)",
                            R"("secret":"x")"}) {
        r = call(http::Method::Put, "/api/v1/admin/integrations/ha_main",
                 std::string(R"({"settings":{"type":"home_assistant",)") + bad + R"(},"revision":)" +
                     std::to_string(revision) + "}");
        EXPECT_EQ(r.status, 422) << bad;
    }
    r = call(http::Method::Put, "/api/v1/admin/integrations/ha_main",
             R"({"settings":{"type":"ble_hid","device_name":"x"},"revision":)" + std::to_string(revision) + "}");
    EXPECT_EQ(r.status, 422);
    r = call(http::Method::Put, "/api/v1/admin/integrations/ha_main", R"({"name":"x","revision":1})");
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/integrations/nope").status, 404);
    r = call(http::Method::Get, "/api/v1/admin/integrations/ble");
    EXPECT_EQ(r.json["settings"]["device_name"], "Hubbo");
    EXPECT_FALSE(r.json["settings"]["bonded"].is<bool>()) << "bonds live on the devices";

    // A failed token write leaves no token and says so.
    secrets.failWrites = true;
    r = call(http::Method::Put, "/api/v1/admin/integrations/ha_main",
             R"({"settings":{"type":"home_assistant","access_token":")" + token + R"("},"revision":)" +
                 std::to_string(revision) + "}");
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(r.body.find(token), std::string::npos);
}

TEST_F(ApiTest, TheMqttPasswordIsWriteOnly) {
    const std::string password = "broker-pw-7c1e";
    const auto pumpMqtt = [&] {
        for (int i = 0; i < 3; i++) {
            mqtt.poll(0);
            mqttTransport.poll(0);
        }
    };
    const auto put = [&](const std::string &settings, uint32_t revision, const char *more = "") {
        return call(http::Method::Put, "/api/v1/admin/integrations/mqtt_main",
                    R"({"settings":{"type":"mqtt")" + settings + "}" + more + R"(,"revision":)" +
                        std::to_string(revision) + "}");
    };
    Reply r = call(http::Method::Get, "/api/v1/admin/integrations/mqtt_main");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["type"], "mqtt");
    EXPECT_EQ(r.json["enabled"], false);
    std::string settings;
    serializeJson(r.json["settings"], settings);
    EXPECT_EQ(settings, R"({"type":"mqtt","host":"","port":1883,"username":"","has_password":false})");
    uint32_t revision = r.json["revision"];

    r = put(R"(,"host":"192.168.1.20","port":1884,"username":"hubbo","password":")" + password + R"(")", revision,
            R"(,"enabled":true)");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.body.find(password), std::string::npos);
    EXPECT_EQ(r.json["settings"]["host"], "192.168.1.20");
    EXPECT_EQ(r.json["settings"]["port"], 1884);
    EXPECT_EQ(r.json["settings"]["username"], "hubbo");
    EXPECT_EQ(r.json["settings"]["has_password"], true);
    revision = r.json["revision"];
    std::string stored;
    ASSERT_EQ(secrets.read(api::kMqttPasswordKey, &stored), auth::SecretRead::Ok);
    EXPECT_EQ(stored, password);
    pumpMqtt();  // the bridge got the settings and the password
    EXPECT_STREQ(mqtt.state(), "connected");
    EXPECT_EQ(mqttTransport.lastOptions().host, "192.168.1.20");
    EXPECT_EQ(mqttTransport.lastOptions().port, 1884);
    EXPECT_EQ(mqttTransport.lastOptions().username, "hubbo");
    EXPECT_EQ(mqttTransport.lastOptions().password, password);
    EXPECT_EQ(mqtt.detail().find(password), std::string::npos);
    const std::string backup = call(http::Method::Get, "/api/v1/admin/backup").body;
    EXPECT_EQ(backup.find(password), std::string::npos);
    EXPECT_NE(backup.find("192.168.1.20"), std::string::npos);

    // Omitted: kept. null: cleared. A new host without a password: cleared.
    r = put(R"(,"port":1883)", revision);
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["settings"]["has_password"], true);
    r = put(R"(,"password":null)", r.json["revision"]);
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["settings"]["has_password"], false);
    r = put(R"(,"password":")" + password + R"(")", r.json["revision"]);
    ASSERT_EQ(r.json["settings"]["has_password"], true);
    r = put(R"(,"host":"fd00::10")", r.json["revision"]);  // IPv6
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["settings"]["has_password"], false);
    EXPECT_EQ(secrets.read(api::kMqttPasswordKey, &stored), auth::SecretRead::NotFound);
    r = put(R"(,"host":"192.168.1.20","password":")" + password + R"(")", r.json["revision"]);
    ASSERT_EQ(r.json["settings"]["has_password"], true) << "a new host with its password keeps it";
    revision = r.json["revision"];

    // Bad shapes and another integration's settings.
    for (const char *bad : {R"(,"port":0)", R"(,"port":65536)", R"(,"port":"1883")", R"(,"host":"a b")",
                            R"(,"host":"mqtt://x")", R"(,"host":"192.168.1.5:1883")", R"(,"username":"a\u0001")", R"(,"password":"")",
                            R"(,"password":5)", R"(,"access_token":"x")", R"(,"url":"http://x")"}) {
        r = put(bad, revision);
        EXPECT_EQ(r.status, 422) << bad;
        EXPECT_EQ(r.body.find(password), std::string::npos);
    }
    r = put(R"(,"username":")" + std::string(65, 'u') + R"(")", revision);
    EXPECT_EQ(r.status, 422);
    EXPECT_EQ(r.json["error"]["details"][0]["path"], "/settings/username");
    r = call(http::Method::Put, "/api/v1/admin/integrations/mqtt_main",
             R"({"settings":{"type":"home_assistant","url":""},"revision":)" + std::to_string(revision) + "}");
    EXPECT_EQ(r.status, 422);
    EXPECT_EQ(r.json["error"]["details"][0]["code"], "integration_mismatch");

    // A reset whose second erase fails: 503, the config stays, and the HA
    // client follows the token already erased.
    connectHa();
    secrets.failEraseKey = api::kMqttPasswordKey;
    r = call(http::Method::Post, "/api/v1/admin/reset", "");
    EXPECT_EQ(r.status, 503) << dump(r);
    EXPECT_EQ(secrets.read(api::kHaTokenKey, &stored), auth::SecretRead::NotFound);
    EXPECT_EQ(secrets.read(api::kMqttPasswordKey, &stored), auth::SecretRead::Ok);
    pumpHa();
    EXPECT_EQ(haClient.state(), integrations::HaState::NotConfigured) << "re-applied without the erased token";
    EXPECT_EQ(hub.configService().findIntegration(kMqttIntegrationId)->host, "192.168.1.20");
    secrets.failEraseKey.clear();

    // A reset to defaults with another host forgets the password and the
    // bridge follows.
    ASSERT_EQ(call(http::Method::Post, "/api/v1/admin/reset", "").status, 200);
    EXPECT_EQ(secrets.read(api::kMqttPasswordKey, &stored), auth::SecretRead::NotFound);
    EXPECT_STREQ(mqtt.state(), "disabled");

    // The reset config is the first-boot one: the built-ins plus off:tap -> system off.
    Reply globals = call(http::Method::Get, "/api/v1/global-bindings");
    ASSERT_EQ(globals.status, 200) << dump(globals);
    JsonObjectConst bindings = globals.json["bindings"];
    EXPECT_EQ(bindings.size(), 1u) << dump(globals);
    EXPECT_EQ(bindings["off:tap"][0]["integration_id"], "system");
    EXPECT_EQ(bindings["off:tap"][0]["command_id"], "off");
    EXPECT_TRUE(bindings["off:tap"][0]["device_id"].isNull());
}

TEST_F(ApiTest, DiscoveryIsAsyncAndFiltered) {
    EXPECT_EQ(call(http::Method::Post, "/api/v1/admin/integrations/ble/discover").status, 404);
    EXPECT_EQ(call(http::Method::Post, "/api/v1/admin/integrations/ha_main/discover").status, 503);  // not connected
    Reply r = call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover");
    EXPECT_EQ(r.json["state"], "idle");
    connectHa();

    r = call(http::Method::Post, "/api/v1/admin/integrations/ha_main/discover");
    EXPECT_EQ(r.status, 202);
    EXPECT_EQ(r.json["state"], "running");
    EXPECT_EQ(call(http::Method::Post, "/api/v1/admin/integrations/ha_main/discover").json["state"], "running");
    pumpHa();
    r = call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover");
    ASSERT_EQ(r.json["state"], "done") << dump(r);
    const size_t total = r.json["total"];
    EXPECT_GT(total, 3u);
    EXPECT_EQ(r.json["truncated"], 0);
    EXPECT_TRUE(r.json["elapsed_ms"].is<uint32_t>());
    // Stays until the next discovery.
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover").json["total"], total);

    bool sawRegistered = false;
    for (JsonVariant c : r.json["candidates"].as<JsonArray>()) {
        if (c["provider"]["entity_id"] == "media_player.denon") {
            EXPECT_EQ(c["registered_device_id"], "dev_denon");
            EXPECT_EQ(c["supported"], true);
            EXPECT_GT(c["command_ids"].size(), 0u);
            sawRegistered = true;
        }
        // ha::suggestedKind() (HaSuggestedKind covers the mapping).
        EXPECT_TRUE(c["suggested_kind"].is<const char *>()) << c["provider"]["entity_id"].as<std::string>();
    }
    EXPECT_TRUE(sawRegistered);

    r = call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover?domain=light");
    for (JsonVariant c : r.json["candidates"].as<JsonArray>()) EXPECT_EQ(c["domain"], "light");
    r = call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover?q=DENON");
    EXPECT_GE(r.json["candidates"].size(), 1u);
    for (JsonVariant c : r.json["candidates"].as<JsonArray>()) {
        EXPECT_NE(c["provider"]["entity_id"].as<std::string>().find("denon"), std::string::npos);
    }
    // Paging.
    r = call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover?limit=2");
    EXPECT_EQ(r.json["candidates"].size(), 2u);
    const std::string cursor = r.json["next_cursor"];
    r = call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover?limit=100&cursor=" + cursor);
    EXPECT_EQ(r.json["candidates"].size(), total - 2);
    EXPECT_TRUE(r.json["next_cursor"].isNull());
    // available=true: only registrable candidates, filtered before paging
    // (a page of 1 is never empty while some are left).
    size_t registrable = 0;
    r = call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover?limit=100");
    for (JsonVariant c : r.json["candidates"].as<JsonArray>()) {
        registrable += c["supported"] == true && c["registered_device_id"].isNull() ? 1 : 0;
    }
    size_t walked = 0;
    for (std::string next = "0"; !next.empty(); walked++) {
        r = call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover?available=true&limit=1&cursor=" + next);
        ASSERT_EQ(r.status, 200) << dump(r);
        if (r.json["candidates"].size() == 0) break;
        ASSERT_EQ(r.json["candidates"].size(), 1u) << dump(r);
        EXPECT_EQ(r.json["candidates"][0]["supported"], true);
        EXPECT_TRUE(r.json["candidates"][0]["registered_device_id"].isNull());
        next = r.json["next_cursor"].isNull() ? "" : r.json["next_cursor"].as<std::string>();
    }
    EXPECT_GT(registrable, 0u);
    EXPECT_EQ(walked, registrable);
    EXPECT_LT(registrable, total) << "the fixtures have registered entities to hide";
    for (const char *bad : {"?limit=0", "?limit=101", "?cursor=abc", "?domain=sensor", "?available=yes"}) {
        EXPECT_EQ(call(http::Method::Get, std::string("/api/v1/admin/integrations/ha_main/discover") + bad).status,
                  422)
            << bad;
    }
    r = call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover?domain=sensor");
    EXPECT_EQ(r.json["error"]["details"][0]["message"], "domain must be media_player, light, switch, fan, scene, script, cover, button, input_boolean, "
              "humidifier, select or remote");

    // Cancel drops the candidates.
    EXPECT_EQ(call(http::Method::Delete, "/api/v1/admin/integrations/ha_main/discover").status, 204);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover").json["state"], "idle");
    call(http::Method::Post, "/api/v1/admin/integrations/ha_main/discover");
    EXPECT_EQ(call(http::Method::Delete, "/api/v1/admin/integrations/ha_main/discover").status, 204);
    pumpHa();
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover").json["state"], "idle");

    // A dropped connection fails a running discovery.
    call(http::Method::Post, "/api/v1/admin/integrations/ha_main/discover");
    haTransport.dropConnection();
    pumpHa(1);
    r = call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover");
    EXPECT_EQ(r.json["state"], "failed") << dump(r);
    EXPECT_FALSE(r.json["error"].as<std::string>().empty());
}

// A fan's speed step reaches the catalog's `step` (it was always 1).
TEST_F(ApiTest, FanSpeedStepReachesTheCatalog) {
    sim::FakeHaEntity fan;
    fan.info.entityId = "fan.ceiling_fan";
    fan.info.supportedFeatures = 49;  // SET_SPEED | TURN_OFF | TURN_ON, 3 speeds
    fan.info.percentageStep = 33.333333333333336;
    fan.uniqueId = "uid_fan";
    haServer.addEntity(fan);
    hub.homeAssistant().addEntity(fan.info);  // the engine's scripted HA serves its catalog
    connectHa();
    ASSERT_EQ(details("fan.ceiling_fan").json["state"], "done");
    Reply r = call(http::Method::Post, "/api/v1/admin/devices",
                   R"({"name":"Ceiling Fan","integration_id":"ha_main","kind":"fan",
                       "provider":{"type":"home_assistant","entity_id":"fan.ceiling_fan"}})");
    ASSERT_EQ(r.status, 201) << dump(r);
    r = call(http::Method::Get, "/api/v1/devices/" + r.json["id"].as<std::string>() + "/capabilities");
    ASSERT_EQ(r.status, 200);
    int checked = 0;
    for (JsonVariant c : r.json["commands"].as<JsonArray>()) {
        for (JsonVariant p : c["params"].as<JsonArray>()) {
            const std::string name = p["name"];
            if (name != "percentage") continue;
            EXPECT_EQ(p["step"], 33) << c["command_id"].as<std::string>();
            EXPECT_EQ(p["unit"], "%");
            checked++;
        }
    }
    EXPECT_EQ(checked, 2) << "turn_on and set_percentage: " << dump(r);
}

TEST_F(ApiTest, YamlRemoteGetsTheRemoteDomainsServices) {
    sim::FakeHaEntity remote;
    remote.info.entityId = "remote.bedroom_tv";
    remote.registered = false;  // YAML-only: no platform
    haServer.addEntity(remote);
    hub.homeAssistant().addEntity(remote.info);
    connectHa();
    ASSERT_EQ(details("remote.bedroom_tv").json["state"], "done");
    Reply r = call(http::Method::Post, "/api/v1/admin/devices",
                   R"({"name":"Bedroom TV","integration_id":"ha_main","kind":"other",
                       "provider":{"type":"home_assistant","entity_id":"remote.bedroom_tv"}})");
    ASSERT_EQ(r.status, 201) << dump(r);
    const std::string id = r.json["id"].as<std::string>();
    r = call(http::Method::Get, "/api/v1/devices/" + id + "/capabilities");
    ASSERT_EQ(r.status, 200);
    std::vector<std::string> commands, services;
    for (JsonVariant c : r.json["commands"].as<JsonArray>()) {
        commands.push_back(c["command_id"].as<std::string>());
        for (JsonVariant s : c["services"].as<JsonArray>()) services.push_back(s["service"].as<std::string>());
    }
    EXPECT_EQ(commands, (std::vector<std::string>{"remote.turn_on", "remote.turn_off", "remote.toggle", "call_service"}));
    EXPECT_EQ(services, (std::vector<std::string>{"remote.send_command", "remote.delete_command",
                                                  "remote.learn_command"}));
    const std::string path = "/api/v1/devices/" + id + "/commands/call_service/execute";
    r = call(http::Method::Post, path,
             R"({"params":{},"call":{"service":"remote.send_command","data":{"command":"DPAD_UP"}}})");
    EXPECT_EQ(r.status, 200) << dump(r);
    r = call(http::Method::Post, path, R"({"params":{},"call":{"service":"homeassistant.stop"}})");
    EXPECT_EQ(r.status, 422) << dump(r);
    EXPECT_EQ(r.json["error"]["details"][0]["path"], "/call/service");
}

TEST_F(ApiTest, DiscoverySearchDecodesTheQuery) {
    sim::FakeHaEntity office;
    office.info.entityId = "light.office_ceiling";
    office.name = "Office Lights";
    office.uniqueId = "uid_office";
    haServer.addEntity(office);
    connectHa();
    ASSERT_EQ(discover().json["state"], "done");
    for (const char *q : {"office%20lights", "office+lights", "OFFICE%20LIGHTS"}) {
        Reply r = call(http::Method::Get, std::string("/api/v1/admin/integrations/ha_main/discover?q=") + q);
        ASSERT_EQ(r.status, 200) << q;
        ASSERT_EQ(r.json["candidates"].size(), 1u) << q << " " << dump(r);
        EXPECT_EQ(r.json["candidates"][0]["provider"]["entity_id"], "light.office_ceiling");
    }
    // Decoded before the length check: 48 decoded characters are fine.
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover?q=" + std::string(16, 'a') +
                                          std::string(32, '+'))
                  .status,
              200);
    // Other params decode too: ?revision=%31 is revision 1.
    const uint32_t rev = hub.configService().findDevice("dev_denon")->revision;
    const Reply r = call(http::Method::Delete,
                   "/api/v1/admin/devices/dev_denon?revision=%" + std::to_string(30 + rev % 10));
    ASSERT_LT(rev, 10u);
    EXPECT_EQ(r.code(), "device_in_use");  // the revision parsed; the device is referenced
    // Each referrer by name: "Used by <activity name>" or "Used by global buttons".
    ASSERT_GT(r.json["error"]["details"].size(), 0u) << dump(r);
    for (JsonVariantConst d : r.json["error"]["details"].as<JsonArrayConst>()) {
        const std::string path = d["path"].as<std::string>();
        const std::string message = d["message"].as<std::string>();
        if (path.rfind("/global_bindings", 0) == 0) {
            EXPECT_EQ(message, "Used by global buttons");
        } else {
            const size_t start = std::string_view("/activities/").size();
            const std::string id = path.substr(start, path.find('/', start) - start);
            EXPECT_EQ(message, "Used by " + hub.configService().findActivity(id)->name) << path;
        }
    }
}

TEST_F(ApiTest, NewHomeAssistantSettingsDropTheOldDiscovery) {
    connectHa();
    ASSERT_EQ(discover().json["state"], "done");
    ASSERT_EQ(details("light.family_room").json["state"], "done");
    ASSERT_NE(hub.configService().findIntegration("ha_main"), nullptr);
    const uint32_t revision = hub.configService().findIntegration("ha_main")->revision;
    // A rename keeps it; another URL (another HA) drops it.
    Reply r = call(http::Method::Put, "/api/v1/admin/integrations/ha_main",
                   R"({"name":"HA","revision":)" + std::to_string(revision) + "}");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover").json["state"], "done");
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/integrations/ha_main/entities/light.family_room/details")
                  .json["state"],
              "done");
    r = call(http::Method::Put, "/api/v1/admin/integrations/ha_main",
             R"({"settings":{"type":"home_assistant","url":"http://other.local:8123"},"revision":)" +
                 std::to_string(r.json["revision"].as<uint32_t>()) + "}");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/admin/integrations/ha_main/discover").json["state"], "idle");
    r = call(http::Method::Post, "/api/v1/admin/devices",
             R"({"name":"Old","integration_id":"ha_main",
                 "provider":{"type":"home_assistant","entity_id":"light.family_room"}})");
    EXPECT_EQ(r.code(), "details_required") << "nothing is registered from the old HA's details";
    // ca_cert_pem is gone from the API.
    r = call(http::Method::Put, "/api/v1/admin/integrations/ha_main",
             R"({"settings":{"type":"home_assistant","ca_cert_pem":null},"revision":)" +
                 std::to_string(hub.configService().findIntegration("ha_main")->revision) + "}");
    EXPECT_EQ(r.status, 422);
}

namespace {

// The `command_id`s of one press list ("tap"), "" joined by spaces, with
// each action's `step` if it has one ("light.brightness_step:26").
std::string actions(JsonVariantConst list) {
    std::string out;
    for (JsonVariantConst a : list.as<JsonArrayConst>()) {
        if (!out.empty()) out += " ";
        out += a["command_id"].as<std::string>();
        if (!a["params"]["step"].isNull()) out += ":" + std::to_string(a["params"]["step"].as<int>());
    }
    return out;
}

// One slot's actions as "button: tap / hold / release | plus: ... | minus: ...".
std::string slotText(JsonVariantConst slot) {
    std::string out;
    for (const char *press : {"button", "plus", "minus"}) {
        JsonVariantConst p = slot[press];
        if (!out.empty()) out += " | ";
        out += std::string(press) + ": " + actions(p["tap"]) + " / " + actions(p["hold"]) + " / " +
               actions(p["release"]);
    }
    return out;
}

const char *const kNoActions = "button:  /  /  | plus:  /  /  | minus:  /  / ";

// A PUT body: `light1` (a JSON value: "null" or a quoted id) as Light 1,
// the others null.
std::string smartBody(const std::string &light1, uint32_t revision) {
    return R"({"revision":)" + std::to_string(revision) + R"(,"slots":{"light_1":)" + light1 +
           R"(,"light_2":null,"switch_1":null,"switch_2":null}})";
}

}  // namespace

TEST_F(ApiTest, SmartButtonsReplaceWithRevisions) {
    Reply r = call(http::Method::Get, "/api/v1/smart-buttons");
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["slots"].size(), 4u);
    for (const char *slot : {"light_1", "light_2", "switch_1", "switch_2"}) {
        EXPECT_FALSE(r.json["actions"][slot].isNull()) << slot;
    }
    const uint32_t revision = r.json["revision"];

    r = call(http::Method::Put, "/api/v1/smart-buttons", smartBody(R"("dev_lights")", revision));
    ASSERT_EQ(r.status, 200) << dump(r);
    EXPECT_EQ(r.json["slots"]["light_1"], "dev_lights");
    EXPECT_TRUE(r.json["slots"]["light_2"].isNull());
    EXPECT_EQ(slotText(r.json["actions"]["light_2"]), kNoActions) << "a null slot";
    const uint32_t next = r.json["revision"];
    EXPECT_GT(next, revision);
    EXPECT_EQ(dump(call(http::Method::Get, "/api/v1/smart-buttons")), dump(r)) << "GET reads what PUT answered";

    // The old base is stale.
    r = call(http::Method::Put, "/api/v1/smart-buttons", smartBody("null", revision));
    EXPECT_EQ(r.status, 409) << dump(r);
    EXPECT_EQ(r.code(), "revision_conflict");
    EXPECT_EQ(r.json["error"]["current_revision"], next);

    // A slot is a registered device.
    r = call(http::Method::Put, "/api/v1/smart-buttons", smartBody(R"("dev_nope")", next));
    EXPECT_EQ(r.status, 422) << dump(r);
    EXPECT_EQ(r.json["error"]["details"][0]["path"], "/slots/light_1") << dump(r);
    EXPECT_EQ(r.json["error"]["details"][0]["code"], "unknown_device") << dump(r);

    // A slot's device can't be deleted from under it.
    r = call(http::Method::Delete, "/api/v1/admin/devices/dev_lights?revision=" +
                                       std::to_string(hub.configService().findDevice("dev_lights")->revision));
    EXPECT_EQ(r.status, 409) << dump(r);
    EXPECT_EQ(r.code(), "device_in_use");
    bool named = false;
    for (JsonVariant d : r.json["error"]["details"].as<JsonArray>()) {
        if (d["path"] == "/smart_buttons/slots/light_1") {
            named = true;
            EXPECT_EQ(d["message"], "Used by smart buttons");
        }
    }
    EXPECT_TRUE(named) << dump(r);
}

TEST_F(ApiTest, StatusShowsTheSelectedSlot) {
    Reply r = call(http::Method::Get, "/api/v1/status");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.json["selected_slot"], "light_1") << "SimRuntime's default, as after a reboot";
}

TEST_F(ApiTest, SmartButtonActionsByKindFromTheOfferedCommands) {
    // Each as an HA device of `kind` over an entity the scripted HA serves,
    // by name (`ids`: the assigned id).
    std::map<std::string, std::string> ids{{"dev_apple_tv", "dev_apple_tv"}};
    auto add = [&](const std::string &name, const char *kind, ha::EntityInfo info) {
        hub.homeAssistant().addEntity(info);
        const config::Outcome o = hub.configService().createDevice(sim::haDevice("", name, kind, info.entityId));
        ASSERT_TRUE(o.ok()) << errorCodeName(o.code);
        ids[name] = o.id;
    };
    auto entity = [](const char *id, uint32_t features) {
        ha::EntityInfo e;
        e.entityId = id;
        e.supportedFeatures = features;
        return e;
    };
    ha::EntityInfo dimmable = entity("light.lamp", 0);
    dimmable.supportedColorModes = {"brightness"};
    add("dev_lamp", "light", dimmable);
    ha::EntityInfo onoff = entity("light.porch", 0);
    onoff.supportedColorModes = {"onoff"};
    add("dev_porch", "light", onoff);
    add("dev_fan", "fan", entity("fan.ceiling", 49));       // SET_SPEED | TURN_OFF | TURN_ON
    add("dev_plain_fan", "fan", entity("fan.plain", 48));   // TURN_OFF | TURN_ON
    add("dev_blinds", "cover", entity("cover.blinds", 15));  // OPEN | CLOSE | SET_POSITION | STOP
    add("dev_garage", "cover", entity("cover.garage", 11));  // OPEN | CLOSE | STOP
    add("dev_kettle", "switch", entity("switch.kettle", 0));
    add("dev_guest", "switch", entity("input_boolean.guest", 0));  // the kind decides, not the domain
    add("dev_movie", "scene", entity("scene.movie", 0));

    // The preview for the device, and the same in Light 1 once saved.
    auto slotActions = [&](const std::string &name) {
        const std::string id = ids.at(name);
        Reply preview = call(http::Method::Get, "/api/v1/smart-buttons/actions?device_id=" + id);
        EXPECT_EQ(preview.status, 200) << id << " " << dump(preview);
        for (JsonVariant a : preview.json["button"]["tap"].as<JsonArray>()) {
            EXPECT_EQ(a["integration_id"], "ha_main");
            EXPECT_EQ(a["device_id"], id);
        }
        const uint32_t revision = call(http::Method::Get, "/api/v1/smart-buttons").json["revision"];
        Reply r = call(http::Method::Put, "/api/v1/smart-buttons", smartBody("\"" + id + "\"", revision));
        EXPECT_EQ(r.status, 200) << id << " " << dump(r);
        EXPECT_EQ(slotText(r.json["actions"]["light_1"]), slotText(preview.json)) << id << ": saved as previewed";
        return slotText(preview.json);
    };
    EXPECT_EQ(slotActions("dev_lamp"),
              "button: light.turn_on / light.turn_off /  | "
              "plus: light.brightness_step:26 /  /  | "
              "minus: light.brightness_step:-26 /  / ");
    EXPECT_EQ(slotActions("dev_porch"), "button: light.turn_on / light.turn_off /  | plus:  /  /  | minus:  /  / ")
        << "no brightness: no +/-";
    EXPECT_EQ(slotActions("dev_fan"),
              "button: fan.turn_on / fan.turn_off /  | plus: fan.increase_speed /  /  | "
              "minus: fan.decrease_speed /  / ");
    EXPECT_EQ(slotActions("dev_plain_fan"), "button: fan.turn_on / fan.turn_off /  | plus:  /  /  | minus:  /  / ")
        << "no speed: no +/-";
    EXPECT_EQ(slotActions("dev_blinds"),
              "button: cover.open_cover / cover.close_cover /  | "
              "plus: cover.position_step:10 / cover.open_cover / cover.stop_cover | "
              "minus: cover.position_step:-10 / cover.close_cover / cover.stop_cover");
    EXPECT_EQ(slotActions("dev_garage"),
              "button: cover.open_cover / cover.close_cover /  | plus: cover.open_cover /  /  | "
              "minus: cover.close_cover /  / ")
        << "no position: open and close";
    EXPECT_EQ(slotActions("dev_kettle"),
              "button: switch.turn_on / switch.turn_off /  | plus:  /  /  | minus:  /  / ");
    EXPECT_EQ(slotActions("dev_guest"),
              "button: input_boolean.turn_on / input_boolean.turn_off /  | plus:  /  /  | minus:  /  / ");
    EXPECT_EQ(slotActions("dev_movie"), kNoActions) << "another kind";
    EXPECT_EQ(slotActions("dev_apple_tv"), kNoActions) << "not Home Assistant";

    Reply r = call(http::Method::Get, "/api/v1/smart-buttons/actions?device_id=dev_nope");
    EXPECT_EQ(r.status, 404) << dump(r);
    r = call(http::Method::Get, "/api/v1/smart-buttons/actions");
    EXPECT_EQ(r.status, 422);
    EXPECT_EQ(r.json["error"]["details"][0]["path"], "/device_id");
    EXPECT_EQ(r.json["error"]["details"][0]["code"], "required");
    for (const char *bad : {"lamp", "dev_", "dev_Lamp", "dev_a.b"}) {
        r = call(http::Method::Get, std::string("/api/v1/smart-buttons/actions?device_id=") + bad);
        EXPECT_EQ(r.status, 422) << bad;
        EXPECT_EQ(r.json["error"]["details"][0]["code"], "invalid_id") << bad;
    }
}
