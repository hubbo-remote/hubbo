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
#include "fixtures.h"
#include "hub.h"
#include "hubbo/api/api.h"
#include "hubbo/config/service.h"

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
        router.setAllowedHosts({"localhost", "127.0.0.1"});
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
        if (!cookie.empty()) {
            headers.items["cookie"] = "hubbo_session=" + cookie;
            if (!csrf.empty()) headers.items["x-hubbo-csrf"] = csrf;
        }
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
        r.clientKey = 42;
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
        c.csrfValid = true;
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
        csrf = r.json["csrf_token"].as<std::string>();
    }

    sim::SimHub hub;
    ValidationReport seeded;
    SimRuntime runtime{hub};
    auth::MemorySecretStore secrets;
    auth::SoftwareHash hash;
    auth::test::CountingRandom random;
    auth::Credentials creds{secrets, hash, random, auth::kMinPasswordIterations};
    auth::SessionStore sessions{random};
    auth::LoginRateLimiter limiter;
    auth::test::FakeClock clock;
    api::Authenticator authn{creds, sessions, clock};
    api::Api api{api::ApiContext{creds, sessions, limiter, clock, hub.configService(), runtime, {}}};
    http::Router router;

    bool noAuth = true;  // simulated admin unless a test logs in
    std::string token, cookie, csrf;
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
    EXPECT_EQ(call(http::Method::Post, "/api/v1/activities", kActivity, {{"origin", "http://evil.example"}}).status,
              403);
    EXPECT_EQ(call(http::Method::Get, "/api/v1/devices", "", {{"host", "evil.example"}}).status, 421);
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
    // Holds are for bindings only: execute refuses key_down, allows key_tap.
    r = call(http::Method::Post, "/api/v1/devices/dev_apple_tv/commands/key_down/execute",
             R"({"params":{"key":"SELECT"}})");
    EXPECT_EQ(r.status, 422);
    EXPECT_EQ(r.json["error"]["details"][0]["code"], "hold_not_allowed");
    r = call(http::Method::Post, "/api/v1/devices/dev_apple_tv/commands/key_tap/execute",
             R"({"params":{"key":"SELECT"}})");
    EXPECT_EQ(r.status, 200) << dump(r);
    r = call(http::Method::Get, "/api/v1/devices/dev_apple_tv/capabilities");
    for (JsonVariant c : r.json["commands"].as<JsonArray>()) {
        EXPECT_EQ(c["effect"], c["command_id"] == "key_down" ? "hold" : "momentary") << c["command_id"].as<std::string>();
    }
    EXPECT_EQ(call(http::Method::Post, "/api/v1/devices/dev_nope/commands/media_player.turn_on/execute",
                   R"({"params":{}})").status,
              404);
}

TEST_F(ApiTest, AdminDeviceRegistry) {
    Reply r = call(http::Method::Post, "/api/v1/admin/devices",
                   R"({"name":"TV Backlight","integration_id":"ha_main","kind":"switch",
                       "provider":{"type":"home_assistant","entity_id":"switch.tv_backlight"}})");
    ASSERT_EQ(r.status, 201) << r.code();
    const std::string id = r.json["id"].as<std::string>();
    const uint32_t created = r.json["revision"];
    r = call(http::Method::Put, "/api/v1/admin/devices/" + id,
             R"({"name":"Backlight","revision":)" + std::to_string(created) + "}");
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
    EXPECT_EQ(s.json["csrf_token"], csrf);
    EXPECT_EQ(call(http::Method::Post, "/api/v1/auth/setup", R"({"username":"x","password":"another password"})")
                  .status,
              409);

    const std::string savedCsrf = csrf;
    csrf.clear();
    Reply r = call(http::Method::Post, "/api/v1/admin/tokens", R"({"name":"agent"})");
    EXPECT_EQ(r.status, 403);
    EXPECT_EQ(r.code(), "csrf_failed");
    csrf = savedCsrf;
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

TEST_F(ApiTest, LoginRateLimitAndPasswordChange) {
    loginAsAdmin();
    const std::string adminCookie = cookie;
    cookie.clear();
    Reply r;
    for (int i = 0; i < 10 && r.status != 429; i++) {
        r = call(http::Method::Post, "/api/v1/auth/login", R"({"username":"admin","password":"wrong password!"})");
    }
    EXPECT_EQ(r.status, 429);
    EXPECT_FALSE(r.headers["Retry-After"].empty());

    cookie = adminCookie;
    clock.mono += 60ull * 60 * 1000;  // lockouts expire
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

TEST_F(ApiTest, StatusAndUnimplemented) {
    Reply r = call(http::Method::Get, "/api/v1/status");
    ASSERT_EQ(r.status, 200);
    EXPECT_EQ(r.json["auth_mode"], "enforced");
    EXPECT_GT(r.json["integrations"].size(), 0u);
    r = call(http::Method::Get, "/api/v1/remote/buttons");
    EXPECT_EQ(r.json["buttons"].size(), remote::kButtonCount);
    r = call(http::Method::Get, "/api/v1/admin/firmware");
    EXPECT_EQ(r.status, 501);
    EXPECT_EQ(r.code(), "not_implemented");
    EXPECT_EQ(call(http::Method::Post, "/api/v1/admin/firmware/update", "x").status, 501);
}
