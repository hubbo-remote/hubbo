// hub-sim: scenario runner behaviour, simulator routes (without sockets),
// and the loopback / no-auth guard rails.
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>

#include "fixtures.h"
#include "hubbo/config/json.h"
#include "hubbo/storage/state_store.h"
#include "hubbo/storage/storage.h"
#include "http_server.h"
#include "noauth.h"
#include "scenario.h"
#include "showcase.h"
#include "sim_api.h"

using namespace hubbo;
using namespace hubbo::sim;

namespace {

const std::string kSimDefaultName = config::defaultHubName(kSimMac);  // "Hubbo 534D"

// Credentials as hub-sim wires them: loaded by the runner's boot (bootHub()).
struct SimAuth {
    auth::MemorySecretStore secrets;
    auth::SoftwareHash hash;
    HostRandom random;
    auth::Credentials credentials{secrets, hash, random};
};

bool runScript(ScenarioRunner &runner, const std::string &text) {
    std::istringstream in(text);
    return runner.run(in, "test");
}

struct Headers : http::HeaderSource {
    std::string_view header(std::string_view name) const override {
        if (name == "Host" || name == "host") return "127.0.0.1:8780";
        if (name == "Content-Type" || name == "content-type") return "application/json";
        return {};
    }
};

http::Response call(SimApi &api, http::Method method, const char *path, const char *query = "",
                    const char *body = "", const http::HeaderSource *headers = nullptr) {
    static const Headers defaultHeaders;
    http::Request r;
    r.method = method;
    r.path = path;
    r.query = query;
    r.body = body;
    r.headers = headers != nullptr ? headers : &defaultHeaders;
    r.context = api.authenticator().authenticate(r);
    http::Response out;
    api.handle(r, out);
    return out;
}

// Headers carrying a session cookie (Cookie: hubbo_session=...).
struct WithCookie : Headers {
    std::string cookie;
    std::string_view header(std::string_view name) const override {
        return name == "Cookie" || name == "cookie" ? std::string_view(cookie) : Headers::header(name);
    }
};

// First-run setup through the API; the session cookie it sets ("" if none).
WithCookie setUpAdmin(SimApi &api) {
    WithCookie headers;
    const http::Response setup = call(api, http::Method::Post, "/api/v1/auth/setup", "",
                                      R"({"username":"admin","password":"correct horse battery"})");
    for (const auto &[name, value] : setup.headers) {
        if (name == "Set-Cookie") headers.cookie = value.substr(0, value.find(';'));
    }
    return headers;
}

}  // namespace

TEST(Scenario, PassingScript) {
    ScenarioRunner runner;
    EXPECT_TRUE(runScript(runner, R"(
        # comments and blank lines are fine
        tap tv
        expect system - activate activity_id=act_watch_apple_tv -> accepted
        expect ha_main dev_lg_tv media_player.turn_on
        expect_event activity act_watch_apple_tv
        expect_active act_watch_apple_tv
    )")) << runner.error();
}

// A startup that waits for the Apple TV: connected, it goes straight on; not
// connected, it gives up after timeout_ms, logs it and still goes on.
TEST(Scenario, WaitConnectedLogsATimeoutAndGoesOn) {
    ConfigSpec seed = fixtureConfig();
    ActivitySpec &watch = seed.activities[0];
    ASSERT_EQ(watch.id, "act_watch_apple_tv");
    watch.startup.insert(watch.startup.begin(),
                         action("ble", "dev_apple_tv", "wait_connected", {param("timeout_ms", int64_t{2000})}));
    ScenarioRunner runner(nullptr, nullptr, false, &seed);
    EXPECT_TRUE(runScript(runner, R"(
        activate act_watch_apple_tv
        expect ble dev_apple_tv wait_connected timeout_ms=2000 -> accepted
        expect ha_main dev_lg_tv media_player.turn_on -> accepted
        off
        advance 5000
        drain
        ble disconnected
        activate act_watch_apple_tv
        expect ble dev_apple_tv wait_connected timeout_ms=2000 -> pending
        advance 1999
        expect_none
        advance 1
        expect ble dev_apple_tv wait_connected done -> accepted adapter_unavailable
        expect ha_main dev_lg_tv media_player.turn_on -> accepted
    )")) << runner.error();

    // Switching away cancels the wait and frees its slot: flipping
    // activities faster than the timeout never runs the adapter out of them.
    ASSERT_TRUE(runScript(runner, "off\nadvance 5000\ndrain\n")) << runner.error();
    for (int i = 0; i < 10; i++) {
        ASSERT_TRUE(runScript(runner, R"(
            activate act_watch_apple_tv
            expect ble dev_apple_tv wait_connected timeout_ms=2000 -> pending
            activate act_play_xbox
            drain
        )")) << i << ": " << runner.error();
    }
}

TEST(Scenario, FailuresNameTheLine) {
    ScenarioRunner runner;
    EXPECT_FALSE(runScript(runner, "activate act_play_xbox\nexpect ble dev_apple_tv key_down key=SELECT\n"));
    EXPECT_EQ(runner.error(),
              "test:2: expected 'ble dev_apple_tv key_down key=SELECT', got 'ha_main dev_xbox media_player.turn_on -> "
              "accepted'");
}

TEST(Scenario, RejectsUnknownCommandsAndButtons) {
    ScenarioRunner runner;
    EXPECT_FALSE(runner.runLine("jump"));
    EXPECT_EQ(runner.error(), "unknown command 'jump'");
    EXPECT_FALSE(runner.runLine("press bogus"));
    EXPECT_FALSE(runner.runLine("advance soon"));
    EXPECT_FALSE(runner.runLine("hold up"));
}

TEST(Scenario, ExpectNoneAndResult) {
    ScenarioRunner runner;
    EXPECT_TRUE(runner.runLine("off"));
    EXPECT_TRUE(runner.runLine("expect_result no_active_activity"));
    EXPECT_TRUE(runner.runLine("expect_none"));
    EXPECT_TRUE(runner.runLine("activate act_watch_apple_tv"));
    EXPECT_FALSE(runner.runLine("expect_none"));
}

TEST(SimApi, NoAuthCallerIsAdminAndCanDriveTheEngine) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, /*authEnabled=*/false, a.credentials);

    http::Response r = call(api, http::Method::Post, "/api/v1/activities/act_watch_apple_tv/activate");
    EXPECT_EQ(r.status, 200) << r.body;
    EXPECT_NE(r.body.find("\"active_activity_id\":\"act_watch_apple_tv\""), std::string::npos) << r.body;
    EXPECT_STREQ(runner.hub().engine().activeActivityId(), "act_watch_apple_tv");

    r = call(api, http::Method::Get, "/api/v1/status");
    EXPECT_EQ(r.status, 200) << r.body;
    EXPECT_NE(r.body.find("simulator_no_auth"), std::string::npos) << r.body;

    EXPECT_EQ(call(api, http::Method::Post, "/api/v1/activities/act_nope/activate").status, 404);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/remote/up/tap").status, 200);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/remote/up/smash").status, 400);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/advance", "ms=250").status, 200);
    r = call(api, http::Method::Get, "/sim/log");
    EXPECT_NE(r.body.find("key_down key=ARROW_UP"), std::string::npos);

    r = call(api, http::Method::Post, "/sim/script", "", "drain\noff\nexpect ha_main dev_lg_tv media_player.turn_off\n");
    EXPECT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(call(api, http::Method::Post, "/sim/reset").status, 200);
    EXPECT_FALSE(runner.hub().engine().hasActiveActivity());
}

TEST(SimApi, AuthEnabledModeUsesProductionAuth) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, /*authEnabled=*/true, a.credentials);
    const http::Response status = call(api, http::Method::Get, "/api/v1/status");
    EXPECT_EQ(status.status, 401);
    EXPECT_NE(status.body.find("\"unauthenticated\""), std::string::npos);
    EXPECT_EQ(call(api, http::Method::Post, "/api/v1/activities/act_watch_apple_tv/activate").status, 401);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/reset").status, 401);
    EXPECT_FALSE(runner.hub().engine().hasActiveActivity());
}

// An agent with nothing finds the guide: the docs are public on an
// auth-enabled hub-sim, as on the board.
TEST(SimApi, AgentDocsArePublic) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, /*authEnabled=*/true, a.credentials);
    for (const char *path : {"/api/v1/", "/api/v1/openapi.json", "/api/v1/agent.md", "/llms.txt"}) {
        EXPECT_EQ(call(api, http::Method::Get, path).status, 200) << path;
    }
    EXPECT_NE(call(api, http::Method::Get, "/llms.txt").body.find("](/api/v1/agent.md)"), std::string::npos);
    // No web admin here: the bare URL says what /llms.txt does.
    EXPECT_EQ(call(api, http::Method::Get, "/").body, call(api, http::Method::Get, "/llms.txt").body);
    EXPECT_NE(call(api, http::Method::Get, "/api").body.find("\"docs\":\"/api/v1/agent.md\""), std::string::npos);
    EXPECT_NE(call(api, http::Method::Get, "/api/v1/status").body.find("\"docs\":\"/api/v1/agent.md\""),
              std::string::npos);
}

// No Host allowlist (any name or address reaches the hub), but a foreign
// Origin on an unsafe method is refused, even with auth off.
TEST(SimApi, ForeignOriginIsRefused) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, false, a.credentials);
    struct ForeignOrigin : http::HeaderSource {
        std::string_view header(std::string_view name) const override {
            if (name == "Host" || name == "host") return "hubbo.lan";
            if (name == "Origin" || name == "origin") return "http://evil.example";
            return {};
        }
    } headers;
    EXPECT_EQ(call(api, http::Method::Get, "/api/v1/status", "", "", &headers).status, 200);
    const http::Response refused =
        call(api, http::Method::Post, "/api/v1/activities/act_watch_apple_tv/activate", "", "", &headers);
    EXPECT_EQ(refused.status, 403) << refused.body;
    EXPECT_NE(refused.body.find("csrf_failed"), std::string::npos) << refused.body;
    EXPECT_FALSE(runner.hub().engine().hasActiveActivity());
}

TEST(SimSeed, ApiBackupFixtureIsAValidConfig) {
    std::ifstream in(HUBBO_API_FIXTURES_DIR "/admin/backup.json");
    ASSERT_TRUE(in);
    std::stringstream text;
    text << in.rdbuf();
    ConfigSpec seed;
    const config::ParseResult parsed = config::parseBackup(text.str(), &seed);
    ASSERT_EQ(parsed.code, ErrorCode::Ok) << (parsed.report.ok() ? "" : parsed.report.issues()[0].path);
    SimHub hub;
    const ValidationReport report = hub.load(seed);
    ASSERT_TRUE(report.ok()) << errorCodeName(report.issues()[0].code) << " " << report.issues()[0].path;
    EXPECT_NE(hub.configService().findDevice("dev_sonos"), nullptr);
    EXPECT_EQ(hub.activate("act_watch_apple_tv"), ErrorCode::Ok);
}

// `hub-sim serve --showcase`: every entity captured from a real HA is a
// device whose snapshot came through the client and the normalizer, the
// seed survives a backup round trip and the hub's load-time checks, and
// what HA offers that the hub doesn't model stays out.
TEST(SimShowcase, EveryCapturedEntityIsADevice) {
    // The capture is local only (sim/fixtures/local/, gitignored).
    if (!showcaseMissing().empty()) GTEST_SKIP() << "no local HA capture: " << showcaseMissing();
    const ConfigSpec seed = showcaseConfig();
    ConfigSpec back;
    const config::ParseResult parsed = config::parseBackup(config::serializeBackup(seed, {}), &back);
    ASSERT_EQ(parsed.code, ErrorCode::Ok) << (parsed.report.ok() ? "" : parsed.report.issues()[0].path);

    SimHub hub(nullptr, nullptr, /*realHomeAssistant=*/true);
    hub.haServer().enableShowcase();
    const ValidationReport report = hub.load(back);
    ASSERT_TRUE(report.ok()) << errorCodeName(report.issues()[0].code) << " " << report.issues()[0].path;

    for (const FakeHaEntity &e : showcaseHaEntities()) {
        const auto d = std::find_if(hub.config().devices.begin(), hub.config().devices.end(),
                                    [&](const DeviceSpec &x) { return x.provider.entityId == e.info.entityId; });
        ASSERT_NE(d, hub.config().devices.end()) << e.info.entityId;
        ASSERT_TRUE(d->haCapabilities) << e.info.entityId;
        EXPECT_EQ(d->haCapabilities->platform, e.platform) << e.info.entityId;
        if (ha::entityDomain(e.info.entityId) != ha::EntityDomain::Fan) continue;
        // A fan's preset_modes and percentage_step come through the client.
        JsonDocument attrs;
        ASSERT_EQ(deserializeJson(attrs, e.attributesJson), DeserializationError::Ok);
        EXPECT_EQ(d->kind, "fan");
        EXPECT_EQ(d->haCapabilities->presetModes.empty(), attrs["preset_modes"].as<JsonArrayConst>().size() == 0)
            << e.info.entityId;
        EXPECT_EQ(d->haCapabilities->percentageStep > 0, (attrs["percentage_step"] | 0.0) >= 1) << e.info.entityId;
    }
    EXPECT_EQ(hub.configService().findDevice("dev_shield")->profile->id, "hubbo:android-tv");
    // The capture has no remote: the fake's, with HA's remote services.
    const auto remote = std::find_if(hub.config().devices.begin(), hub.config().devices.end(),
                                     [](const DeviceSpec &x) { return x.provider.entityId == "remote.living_room_tv"; });
    ASSERT_NE(remote, hub.config().devices.end());
    ASSERT_TRUE(remote->haCapabilities);
    EXPECT_TRUE(std::any_of(remote->haCapabilities->customServices.begin(),
                            remote->haCapabilities->customServices.end(),
                            [](const ha::SnapshotCustomService &c) { return c.service == "remote.send_command"; }));
    for (const ActivitySpec &a : hub.config().activities) EXPECT_EQ(hub.activate(a.id), ErrorCode::Ok) << a.id;
}

// `serve --showcase --ha-token`, as the API sees it: every captured HA
// device is available, and each whose integration has services the hub
// can't type (a webOS TV's button) offers a Home Assistant action without
// a re-link.
TEST(SimShowcase, CustomServicesOfferAHomeAssistantAction) {
    if (!showcaseMissing().empty()) GTEST_SKIP() << "no local HA capture: " << showcaseMissing();
    SimAuth a;
    a.secrets.write(api::kHaTokenKey, "fake-ha-token");
    const ConfigSpec seed = showcaseConfig();
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, &seed, nullptr, &a.credentials,
                          /*realHomeAssistant=*/true);
    runner.hub().haServer().enableShowcase();
    SimApi api(runner, false, a.credentials, nullptr, &a.secrets);
    runner.hub().advance(10);
    size_t offering = 0;
    for (const DeviceSpec &d : runner.hub().config().devices) {
        if (d.provider.type != kHomeAssistantType) continue;
        const std::string path = "/api/v1/devices/" + d.id;
        EXPECT_NE(call(api, http::Method::Get, path.c_str()).body.find(R"("availability":"available")"),
                  std::string::npos)
            << d.id;
        const bool custom = d.haCapabilities && !d.haCapabilities->customServices.empty();
        const http::Response caps = call(api, http::Method::Get, (path + "/capabilities").c_str());
        EXPECT_EQ(caps.body.find(R"("command_id":"call_service")") != std::string::npos, custom) << d.id;
        offering += custom;
    }
    EXPECT_GT(offering, 0u);
}

TEST(SimSeed, ServeRestartKeepsConfig) {
    storage::MemoryStorage disk;  // stands in for --data DIR
    {
        ScenarioRunner first(nullptr, &disk, /*pinned=*/true);
        ActivitySpec a;
        a.name = "Reboot Test";
        ASSERT_TRUE(first.hub().configService().createActivity(a).ok());
    }
    ScenarioRunner second(nullptr, &disk, /*pinned=*/true);  // "restart"
    EXPECT_NE(second.hub().configService().findActivity("act_reboot_test"), nullptr);
    second.runLine("reset");  // --reset: back to the seed
    EXPECT_EQ(second.hub().configService().findActivity("act_reboot_test"), nullptr);
}

// The hub's name is kept across a restart, and a reset (`serve --reset`,
// POST /sim/reset) takes it back to the default, "Hubbo 534D" (kSimMac).
TEST(SimSeed, AResetForgetsTheHubName) {
    storage::MemoryStorage disk;  // stands in for --data DIR
    {
        ScenarioRunner first(nullptr, &disk, /*pinned=*/true);
        ASSERT_EQ(first.hub().hubSettings().rename("Den Hub"), ErrorCode::Ok);
    }
    ScenarioRunner second(nullptr, &disk, /*pinned=*/true);  // "restart"
    EXPECT_EQ(second.hub().hubSettings().identity().name, "Den Hub");
    second.runLine("reset");
    EXPECT_EQ(second.hub().hubSettings().identity().name, kSimDefaultName);
    ScenarioRunner third(nullptr, &disk, /*pinned=*/true);
    EXPECT_EQ(third.hub().hubSettings().identity().name, kSimDefaultName);
}

// Every reset path forgets the name, whatever StateStore the hub uses: a
// pinned hub's reset and wipe with an injected store, and a scenario
// (unpinned) reset, which builds a new hub on the same storage.
TEST(SimSeed, EveryResetPathForgetsTheHubName) {
    storage::MemoryStateStore state;
    SimHub hub(nullptr, &state);
    ASSERT_EQ(hub.hubSettings().rename("Den Hub"), ErrorCode::Ok);
    (void)hub.factoryReset(fixtureConfig());
    EXPECT_EQ(hub.hubSettings().identity().name, kSimDefaultName);
    std::string saved;
    EXPECT_EQ(state.get(config::kHubNameKey, &saved), ErrorCode::NotFound);
    ASSERT_EQ(hub.hubSettings().rename("Den Hub"), ErrorCode::Ok);
    hub.wipe(nullptr);
    EXPECT_EQ(hub.hubSettings().identity().name, kSimDefaultName);
    EXPECT_EQ(state.get(config::kHubNameKey, &saved), ErrorCode::NotFound);

    storage::MemoryStorage disk;
    ScenarioRunner runner(nullptr, &disk, /*pinned=*/false);
    ASSERT_EQ(runner.hub().hubSettings().rename("Den Hub"), ErrorCode::Ok);
    ASSERT_TRUE(runner.runLine("reset"));
    EXPECT_EQ(runner.hub().hubSettings().identity().name, kSimDefaultName);
    ScenarioRunner after(nullptr, &disk, /*pinned=*/false);
    EXPECT_EQ(after.hub().hubSettings().identity().name, kSimDefaultName);
}

// POST /sim/reset (a pinned hub's reset) and a wipe forget the run history,
// as the board's restart would: an activity made again shows no old runs.
TEST(SimSeed, EveryResetPathForgetsTheRuns) {
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true);
    SimHub &hub = runner.hub();
    LastRun last;
    ASSERT_EQ(hub.activate("act_watch_apple_tv"), ErrorCode::Ok);
    hub.advance(3000);
    ASSERT_TRUE(hub.engine().lastRunShared(&last));
    ASSERT_TRUE(runner.runLine("reset"));
    hub.advance(100);
    EXPECT_FALSE(hub.engine().lastRunShared(&last));

    ASSERT_EQ(hub.activate("act_watch_apple_tv"), ErrorCode::Ok);
    hub.advance(3000);
    ASSERT_TRUE(hub.engine().lastRunShared(&last));
    hub.wipe(nullptr);
    hub.advance(100);
    EXPECT_FALSE(hub.engine().lastRunShared(&last));
}

// A restart asked for before POST /sim/reset never fires after it.
TEST(SimApi, AResetDropsAPendingRestart) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, /*authEnabled=*/false, a.credentials, nullptr, &a.secrets);
    ASSERT_EQ(call(api, http::Method::Post, "/api/v1/admin/restart").status, 202);
    EXPECT_TRUE(runner.hub().restartPort().pending());
    EXPECT_EQ(call(api, http::Method::Post, "/sim/reset").status, 200);
    EXPECT_FALSE(runner.hub().restartPort().pending());
    runner.hub().advance(api::SimulatedRestart::kRestartMs);
    EXPECT_EQ(runner.hub().restartPort().restarts(), 0);
    // The callback is kept: a new restart still lands.
    ASSERT_EQ(call(api, http::Method::Post, "/api/v1/admin/restart").status, 202);
    runner.hub().advance(api::SimulatedRestart::kRestartMs);
    EXPECT_EQ(runner.hub().restartPort().restarts(), 1);
}

// No-auth hub-sim: an admin set up there survives POST /sim/reset, and a
// factory reset the hub refuses erases nothing. Only an accepted one leaves
// the credentials fresh, until an admin is set up again.
TEST(SimApi, NoAuthKeepsTheAdminUnlessAFactoryResetRuns) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, /*authEnabled=*/false, a.credentials, nullptr, &a.secrets);
    ASSERT_EQ(a.credentials.setupAdmin("hubbo-owner", "correct horse battery"), auth::CredentialResult::Ok);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/reset").status, 200);
    EXPECT_EQ(a.credentials.state(), auth::CredentialState::Ok);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/factory-reset", "ready=0").status, 200);
    EXPECT_EQ(call(api, http::Method::Post, "/api/v1/admin/factory-reset").status, 503);
    runner.hub().advance(api::SimulatedFactoryReset::kRestartMs);
    EXPECT_EQ(a.credentials.state(), auth::CredentialState::Ok);  // nothing erased
    EXPECT_EQ(call(api, http::Method::Post, "/sim/factory-reset", "ready=1").status, 200);
    EXPECT_EQ(call(api, http::Method::Post, "/api/v1/admin/factory-reset").status, 202);
    runner.hub().advance(api::SimulatedFactoryReset::kRestartMs);
    EXPECT_EQ(a.credentials.state(), auth::CredentialState::Fresh);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/reset").status, 200);
    EXPECT_EQ(a.credentials.state(), auth::CredentialState::Fresh);  // a reset doesn't bring an admin back
}

TEST(HttpServer, LoopbackOnlyByDefault) {
    EXPECT_TRUE(isLoopbackAddress("127.0.0.1"));
    EXPECT_TRUE(isLoopbackAddress("127.1.2.3"));
    EXPECT_FALSE(isLoopbackAddress("0.0.0.0"));
    EXPECT_FALSE(isLoopbackAddress("192.168.1.10"));
    EXPECT_FALSE(isLoopbackAddress("localhost"));

    HttpServer server;
    ListenOptions options;
    options.bind = "0.0.0.0";
    options.port = 0;
    std::string error;
    EXPECT_FALSE(server.listen(options, &error));
    EXPECT_NE(error.find("loopback only"), std::string::npos);
}

TEST(NoAuth, BannerText) { EXPECT_STREQ(kNoAuthBanner, "AUTH DISABLED \xE2\x80\x94 SIMULATOR"); }

// hub-sim serve --data over real files: a restart (new runner, same dir)
// brings back the same config byte for byte and the same active activity,
// without running its startup.
TEST(SimSeed, RestartOverRealFilesRoundTrips) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("hubbo_sim_reboot_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    std::string savedConfig;
    std::string savedText;
    {
        storage::StdioStorage disk(dir.string());
        ScenarioRunner first(nullptr, &disk, /*pinned=*/true);
        ActivitySpec a;
        a.name = "Over Real Files";
        a.startup = {haAction("dev_lg_tv", "media_player.turn_on")};
        ASSERT_TRUE(first.hub().configService().createActivity(a).ok());
        ASSERT_EQ(first.hub().activate("act_over_real_files"), ErrorCode::Ok);
        first.hub().advance(ActivePersister::kDefaultDelayMs + 200);
        savedConfig = config::serializeConfig(first.hub().config());
        ASSERT_EQ(disk.read("config.a", &savedText, 1 << 20), ErrorCode::Ok);
        std::string b;
        if (disk.read("config.b", &b, 1 << 20) == ErrorCode::Ok) savedText += "|" + b;
    }
    for (int restart = 0; restart < 2; restart++) {
        storage::StdioStorage disk(dir.string());
        ScenarioRunner again(nullptr, &disk, /*pinned=*/true);
        EXPECT_EQ(config::serializeConfig(again.hub().config()), savedConfig);
        std::string text;
        ASSERT_EQ(disk.read("config.a", &text, 1 << 20), ErrorCode::Ok);
        std::string b;
        if (disk.read("config.b", &b, 1 << 20) == ErrorCode::Ok) text += "|" + b;
        EXPECT_EQ(text, savedText);  // a clean boot writes no slot
        EXPECT_STREQ(again.hub().engine().activeActivityId(), "act_over_real_files");
        EXPECT_TRUE(again.hub().commands().empty());  // restored without startup
    }
    std::filesystem::remove_all(dir);
}

// POST /sim/storage switches the medium's health: edits then fail with 503
// storage_unavailable while activities still switch.
TEST(SimApi, StorageCanBeMadeUnavailable) {
    storage::MemoryStorage memory;
    SwitchableStorage medium(memory);
    SimAuth a;
    ScenarioRunner runner(nullptr, &medium, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, false, a.credentials, &medium);
    const char *create = R"({"name":"Movie","startup":[],"shutdown":[],"bindings":{},"macros":[]})";

    EXPECT_EQ(call(api, http::Method::Post, "/sim/storage", "health=unavailable").status, 200);
    const http::Response refused = call(api, http::Method::Post, "/api/v1/activities", "", create);
    EXPECT_EQ(refused.status, 503) << refused.body;
    EXPECT_NE(refused.body.find("storage_unavailable"), std::string::npos) << refused.body;
    EXPECT_EQ(call(api, http::Method::Post, "/api/v1/activities/act_watch_apple_tv/activate").status, 200);

    EXPECT_EQ(call(api, http::Method::Post, "/sim/storage", "health=bogus").status, 400);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/storage", "health=available").status, 200);
    EXPECT_EQ(call(api, http::Method::Post, "/api/v1/activities", "", create).status, 201);
}

// No-auth mode still reports whether an admin exists: on a fresh hub setup
// works (once), so tokens can then be created.
TEST(SimApi, NoAuthSetupWorksOnAFreshHub) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, false, a.credentials);
    const char *setup = R"({"username":"admin","password":"correct horse battery"})";
    const http::Response first = call(api, http::Method::Post, "/api/v1/auth/setup", "", setup);
    EXPECT_LT(first.status, 300) << first.body;
    const http::Response second = call(api, http::Method::Post, "/api/v1/auth/setup", "", setup);
    EXPECT_EQ(second.status, 409) << second.body;
    EXPECT_NE(second.body.find("setup_already_done"), std::string::npos) << second.body;
}

// /status follows the medium: down -> storage_unavailable -> up -> ok, with
// the same generation and nothing written.
TEST(SimApi, ConfigStateFollowsStorageAtRuntime) {
    storage::MemoryStorage memory;
    SwitchableStorage medium(memory);
    SimAuth a;
    ScenarioRunner runner(nullptr, &medium, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, false, a.credentials, &medium);
    auto stateOf = [&] {
        const http::Response r = call(api, http::Method::Get, "/api/v1/status");
        const size_t at = r.body.find("\"state\":\"");
        return at == std::string::npos ? std::string("?") : r.body.substr(at + 9, r.body.find('"', at + 9) - at - 9);
    };
    const uint32_t generation = runner.hub().configService().generation();
    const auto files = memory.files();
    EXPECT_EQ(stateOf(), "ok");
    call(api, http::Method::Post, "/sim/storage", "health=unavailable");
    EXPECT_EQ(stateOf(), "storage_unavailable");
    call(api, http::Method::Post, "/sim/storage", "health=available");
    EXPECT_EQ(stateOf(), "ok");
    EXPECT_EQ(runner.hub().configService().generation(), generation);
    EXPECT_EQ(memory.files(), files);
}

// Booting with storage down, then getting it back, recovers on the next tick.
TEST(SimApi, BootingWithStorageDownRecovers) {
    storage::MemoryStorage memory;
    {
        ScenarioRunner first(nullptr, &memory, /*pinned=*/true);  // saves the seed
    }
    SwitchableStorage medium(memory);
    medium.setHealth(storage::Health::Unavailable);
    ScenarioRunner runner(nullptr, &medium, /*pinned=*/true);
    EXPECT_EQ(runner.hub().configService().state(), config::ConfigState::StorageUnavailable);
    medium.setHealth(storage::Health::Available);
    runner.hub().advance(200);
    EXPECT_EQ(runner.hub().configService().state(), config::ConfigState::Ok);
    EXPECT_EQ(runner.hub().activate("act_watch_apple_tv"), ErrorCode::Ok);
}

TEST(SimApi, RemotePairScanIsDrivenFromSimRoutes) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, false, a.credentials);
    EXPECT_EQ(call(api, http::Method::Post, "/api/v1/admin/remote/pair").status / 100, 2);
    EXPECT_EQ(runner.hub().remote().status().scan, api::ScanState::Scanning);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/radio/found", "channel=200&address=e7e7e7e7e7").status, 400);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/radio/found", "channel=76&address=e7e7e7e7e7").status, 200);
    const api::RemoteRadioStatus found = runner.hub().remote().status();
    EXPECT_EQ(found.scan, api::ScanState::Found);
    EXPECT_EQ(found.endpoint.channel, 76);
    EXPECT_EQ(found.endpoint.address, 0xe7e7e7e7e7ull);

    // A scan nobody answers times out on hub time.
    EXPECT_EQ(call(api, http::Method::Post, "/api/v1/admin/remote/pair").status / 100, 2);
    const std::string ms = "ms=" + std::to_string(api::kRemoteScanTimeoutMs + 1000);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/advance", ms.c_str()).status, 200);
    EXPECT_EQ(runner.hub().remote().status().scan, api::ScanState::TimedOut);
}

TEST(SimApi, SimulatedPressesShowInRemoteStatusLikeRadioPackets) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, false, a.credentials);
    const auto status = [&] {
        const http::Response r = call(api, http::Method::Get, "/api/v1/remote/status");
        EXPECT_EQ(r.status, 200) << r.body;
        return r.body;
    };
    std::string s = status();
    EXPECT_NE(s.find(R"("last_packet_age_ms":null)"), std::string::npos) << s;  // no packet yet
    EXPECT_NE(s.find(R"("last_event":null)"), std::string::npos) << s;

    ASSERT_EQ(call(api, http::Method::Post, "/sim/advance", "ms=1000").status, 200);
    ASSERT_EQ(call(api, http::Method::Post, "/sim/remote/left/tap").status, 200);
    ASSERT_EQ(call(api, http::Method::Post, "/sim/advance", "ms=250").status, 200);
    s = status();
    EXPECT_NE(s.find(R"("last_packet_age_ms":250)"), std::string::npos) << s;
    EXPECT_NE(s.find(R"("last_event":{"button":"left","kind":"tap","age_ms":)"), std::string::npos) << s;
    EXPECT_NE(s.find(R"("held":[])"), std::string::npos) << s;

    // A press without a release holds (the thumb stays down) until a reset.
    ASSERT_EQ(call(api, http::Method::Post, "/sim/remote/down/press").status, 200);
    ASSERT_EQ(call(api, http::Method::Post, "/sim/advance", "ms=500").status, 200);
    s = status();
    EXPECT_NE(s.find(R"("held":["down"])"), std::string::npos) << s;
    EXPECT_NE(s.find(R"("last_packet_age_ms":)"), std::string::npos) << s;
    ASSERT_EQ(call(api, http::Method::Post, "/sim/reset").status, 200);
    s = status();
    EXPECT_NE(s.find(R"("held":[])"), std::string::npos) << s;
    ASSERT_EQ(call(api, http::Method::Post, "/sim/advance", "ms=1000").status, 200);
    EXPECT_NE(status().find(R"("held":[])"), std::string::npos);  // and doesn't come back
}

// Through the engine: an activity's key goes only to its own device's host;
// a host that isn't connected makes the action unavailable and nothing is
// sent anywhere; an activity change releases what was held on the other host.
TEST(SimHubBluetooth, KeysGoOnlyToTheNamedHost) {
    SimHub hub;
    ConfigSpec c = fixtureConfig();
    c.devices.push_back(bleDevice("dev_shield", "Shield", "streamer"));
    ActivitySpec shield;
    shield.id = "act_watch_shield";
    shield.name = "Watch Shield";
    shield.revision = 1;
    shield.bindings.push_back(bind("play:tap", {bleKey("key_tap", "PLAY_PAUSE", "dev_shield")}));
    c.activities.push_back(shield);
    ASSERT_TRUE(hub.load(c).ok());
    SimulatedBleHosts &hosts = hub.bleHosts();
    hosts.bond("dev_shield", "SHIELD");

    ASSERT_EQ(hub.activate("act_watch_shield"), ErrorCode::Ok);
    hub.advance(100);
    const size_t before = hosts.reports().size();
    hub.tap(remote::Button::Play);
    hub.advance(100);
    ASSERT_EQ(hosts.reports().size(), before + 2);  // down, up
    EXPECT_EQ(hosts.reportDevices()[before], "dev_shield");
    EXPECT_EQ(hosts.reportDevices()[before + 1], "dev_shield");

    hosts.setConnected("dev_shield", false);
    hub.tap(remote::Button::Play);
    hub.advance(100);
    EXPECT_EQ(hosts.reports().size(), before + 2);  // unavailable: nothing sent anywhere
    EXPECT_NE(hub.commands().back().text.find("adapter_unavailable"), std::string::npos)
        << hub.commands().back().text;

    // A key held on the Apple TV is released when the activity changes.
    ASSERT_EQ(hub.activate("act_watch_apple_tv"), ErrorCode::Ok);
    hub.advance(2000);
    hub.press(remote::Button::Up);
    hub.advance(100);
    EXPECT_EQ(hub.ble().heldConsumer("dev_apple_tv"), 0x42);
    ASSERT_EQ(hub.activate("act_watch_shield"), ErrorCode::Ok);
    hub.advance(100);
    EXPECT_EQ(hub.ble().heldConsumer("dev_apple_tv"), 0);
    EXPECT_EQ(hosts.reports().back(), "consumer 0x0000");
    EXPECT_EQ(hosts.reportDevices().back(), "dev_apple_tv");
    hub.release(remote::Button::Up);
}

// Bonds survive a restart, as in the board's NVS: a host pruned by the
// config stays gone, and only a first start gets the fixture's bond.
TEST(SimHubBluetooth, BondsSurviveARestart) {
    storage::MemoryStorage config;
    storage::MemoryStorage nvs;
    {
        SimHub hub(&config, nullptr, false, &nvs);
        hub.boot(fixtureConfig());
        EXPECT_TRUE(hub.bleHosts().status(SimHub::kBondedDevice).bonded);  // first start
        ASSERT_TRUE(hub.load(fixtureConfigWithoutBluetooth()).ok());  // prunes the Apple TV's host
        ASSERT_TRUE(hub.load(fixtureConfig()).ok());
        EXPECT_FALSE(hub.bleHosts().status(SimHub::kBondedDevice).bonded);
        hub.bleHosts().bond(SimHub::kBondedDevice, "Another Apple TV");
        ASSERT_EQ(hub.bleHosts().forget(SimHub::kBondedDevice), integrations::BleHostResult::Accepted);
    }
    SimHub hub(&config, nullptr, false, &nvs);
    hub.boot(fixtureConfig());
    EXPECT_FALSE(hub.bleHosts().status(SimHub::kBondedDevice).bonded);  // still gone
    hub.bleHosts().bond(SimHub::kBondedDevice, SimHub::kBondedHost);
    SimHub again(&config, nullptr, false, &nvs);
    again.boot(fixtureConfig());
    EXPECT_TRUE(again.bleHosts().status(SimHub::kBondedDevice).bonded);
}

// Several Bluetooth hosts, each bonded to its own device, driven from /sim.
TEST(SimApi, BluetoothHostsAreDrivenFromSimRoutes) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, false, a.credentials);
    SimHub &hub = runner.hub();
    SimulatedBleHosts &hosts = hub.bleHosts();
    const SimRuntime runtime(hub);
    const auto line = [&](const char *device) {
        const integrations::BleDeviceState state = integrations::bleDeviceState(hosts.status(device));
        return std::string(state.state) + (state.windowS != 0 ? " " + std::to_string(state.windowS) : "");
    };
    const DeviceSpec shield = bleDevice("dev_shield", "Shield", "streamer");
    const auto pairShield = [&] { return hosts.startPairing("dev_shield", integrations::kBlePairingMinWindowMs); };

    // The fixture world: the Apple TV bonded and connected; the radio working.
    EXPECT_EQ(line("dev_apple_tv"), "connected");
    EXPECT_STREQ(runtime.integrationState("ble"), "connected");
    EXPECT_EQ(runtime.integrationDetail("ble"), "");
    EXPECT_EQ(line("dev_shield"), "not_paired");
    EXPECT_STREQ(runtime.deviceAvailability(shield), "unavailable");

    // Pair a second host; the window runs on hub time.
    ASSERT_EQ(pairShield(), integrations::BleHostResult::Accepted);
    EXPECT_EQ(hosts.startPairing("dev_apple_tv", integrations::kBlePairingMinWindowMs),
              integrations::BleHostResult::Busy);  // one window at a time
    EXPECT_EQ(call(api, http::Method::Post, "/sim/advance", "ms=500").status, 200);
    EXPECT_EQ(hosts.startPairing("dev_shield", integrations::kBlePairingMaxWindowMs),
              integrations::BleHostResult::Accepted);  // its own window: no change
    EXPECT_EQ(call(api, http::Method::Post, "/sim/advance", "ms=1000").status, 200);
    EXPECT_EQ(line("dev_shield"), "pairing 29");
    EXPECT_EQ(call(api, http::Method::Post, "/sim/ble/bond", "device=dev_shield&host=Apple TV").status, 409);  // tied to dev_apple_tv
    EXPECT_EQ(call(api, http::Method::Post, "/sim/ble/bond", "device=dev_shield&host=SHIELD").status, 200);
    EXPECT_EQ(line("dev_shield"), "connected");
    EXPECT_STREQ(runtime.deviceAvailability(shield), "available");
    EXPECT_EQ(call(api, http::Method::Post, "/sim/ble/bond", "device=dev_shield").status, 409);  // no window

    // One host's link drops; the other stays.
    EXPECT_EQ(call(api, http::Method::Post, "/sim/ble/link", "device=dev_shield&up=0").status, 200);
    EXPECT_EQ(line("dev_shield"), "not_connected");
    EXPECT_EQ(line("dev_apple_tv"), "connected");
    EXPECT_EQ(call(api, http::Method::Post, "/sim/ble/link", "device=dev_nope&up=1").status, 404);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/ble/link", "device=dev_shield&up=1").status, 200);
    EXPECT_EQ(line("dev_shield"), "connected");

    // Re-pairing with the host it already had is a success.
    ASSERT_EQ(pairShield(), integrations::BleHostResult::Accepted);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/ble/bond", "device=dev_shield&host=SHIELD").status, 200);
    EXPECT_EQ(line("dev_shield"), "connected");

    // A re-pair that times out keeps the old host; a forget drops only its own bond.
    ASSERT_EQ(pairShield(), integrations::BleHostResult::Accepted);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/advance", "ms=30000").status, 200);
    EXPECT_EQ(line("dev_shield"), "connected");
    ASSERT_EQ(hosts.forget("dev_shield"), integrations::BleHostResult::Accepted);
    EXPECT_EQ(line("dev_shield"), "not_paired");
    EXPECT_EQ(line("dev_apple_tv"), "connected");

    // A first pair that times out is still not paired.
    ASSERT_EQ(pairShield(), integrations::BleHostResult::Accepted);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/advance", "ms=30000").status, 200);
    EXPECT_EQ(line("dev_shield"), "not_paired");

    // The radio.
    EXPECT_EQ(call(api, http::Method::Post, "/sim/ble/radio", "ready=0").status, 200);
    EXPECT_STREQ(runtime.integrationState("ble"), "error");
    EXPECT_EQ(runtime.integrationDetail("ble"), "Bluetooth isn't working");
    EXPECT_EQ(pairShield(), integrations::BleHostResult::NotReady);

    EXPECT_EQ(call(api, http::Method::Post, "/sim/reset").status, 200);
    EXPECT_TRUE(hosts.radioReady());
    EXPECT_EQ(line("dev_apple_tv"), "connected");
}
TEST(SimApi, FirmwareFaultsAreDrivenFromSimRoutes) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, false, a.credentials);
    api::SimulatedFirmwareUpdate &firmware = runner.hub().firmware();
    EXPECT_TRUE(firmware.ready);
    EXPECT_FALSE(firmware.failWrites);

    EXPECT_EQ(call(api, http::Method::Post, "/sim/firmware", "fail=1").status, 200);
    EXPECT_TRUE(firmware.failWrites);
    EXPECT_TRUE(firmware.ready);  // untouched
    EXPECT_EQ(call(api, http::Method::Post, "/sim/firmware", "ready=0").status, 200);
    EXPECT_FALSE(firmware.ready);
    EXPECT_EQ(firmware.install("anything"), api::FirmwareInstallResult::Unavailable);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/firmware").status, 400);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/firmware", "fail=yes").status, 400);

    EXPECT_EQ(call(api, http::Method::Post, "/sim/reset").status, 200);
    EXPECT_TRUE(runner.hub().firmware().ready);
    EXPECT_FALSE(runner.hub().firmware().failWrites);
    EXPECT_EQ(runner.hub().firmware().status().state, api::FirmwareUpdateState::Idle);
}

TEST(SimApi, TheReleaseFeedIsDrivenFromSimRoutes) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, false, a.credentials);
    SimFirmwareReleases &releases = runner.hub().releases();
    EXPECT_FALSE(releases.feed.published);
    EXPECT_EQ(releases.version(), SimFirmwareReleases::kDefaultVersion);

    const auto post = [&](const char *query) { return call(api, http::Method::Post, "/sim/release", query).status; };
    EXPECT_EQ(post("version=1.4.0&running=1.3.0&min_version=1.2.0"), 200);
    EXPECT_TRUE(releases.feed.published);
    EXPECT_EQ(releases.feed.version, "1.4.0");
    EXPECT_EQ(releases.feed.minVersion, "1.2.0");
    EXPECT_EQ(releases.version(), "1.3.0");
    const auto shownVersion = [&] {
        JsonDocument doc;
        deserializeJson(doc, call(api, http::Method::Get, "/api/v1/status").body);
        return std::string(doc["hubbo"]["version"] | "");
    };
    EXPECT_EQ(shownVersion(), "1.3.0");  // the API shows the same version
    EXPECT_EQ(post("min_version="), 200);  // empty: none
    EXPECT_TRUE(releases.feed.minVersion.empty());
    EXPECT_EQ(post("fault=bad_signature"), 200);
    EXPECT_EQ(releases.feed.fault, release::CheckError::BadSignature);
    EXPECT_EQ(post("rolled_back_from=1.5.0"), 200);
    EXPECT_EQ(releases.state().read().rolledBackFrom, "1.5.0");
    EXPECT_EQ(post("none=1"), 200);
    EXPECT_FALSE(releases.feed.published);

    EXPECT_EQ(post("version=v1.4.0"), 400);
    EXPECT_EQ(post("layout=0"), 400);
    EXPECT_EQ(post("fault=slow"), 400);
    EXPECT_EQ(post("none=0"), 400);

    EXPECT_EQ(call(api, http::Method::Post, "/sim/reset").status, 200);
    EXPECT_FALSE(releases.feed.published);
    EXPECT_EQ(releases.feed.fault, release::CheckError::None);
    EXPECT_EQ(releases.version(), SimFirmwareReleases::kDefaultVersion);
    EXPECT_EQ(shownVersion(), SimFirmwareReleases::kDefaultVersion);
    EXPECT_TRUE(releases.state().read().rolledBackFrom.empty());
}

// An install's reboot: the API shows the version the hub now runs.
TEST(SimApi, AnInstalledReleaseIsTheVersionShownAfterTheReboot) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, false, a.credentials);
    SimHub &hub = runner.hub();
    ASSERT_EQ(call(api, http::Method::Post, "/sim/release", "version=1.4.0&running=1.3.0").status, 200);
    ASSERT_EQ(hub.releases().requestCheck(), api::ReleaseCheckResult::Accepted);
    hub.advance(SimFirmwareReleases::kCheckMs);
    ASSERT_EQ(hub.releases().install("1.4.0"), api::ReleaseInstallResult::Accepted);
    hub.advance(SimFirmwareReleases::kDownloadMs + api::SimulatedFirmwareUpdate::kWriteMs +
                api::SimulatedFirmwareUpdate::kRebootMs);
    JsonDocument doc;
    deserializeJson(doc, call(api, http::Method::Get, "/api/v1/status").body);
    EXPECT_STREQ(doc["hubbo"]["version"] | "", "1.4.0");
    EXPECT_EQ(hub.releases().version(), "1.4.0");
}

// As the board: the reboot that finishes an update forgets every session.
TEST(SimApi, FirmwareRebootDropsSessions) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, /*authEnabled=*/true, a.credentials);
    const WithCookie headers = setUpAdmin(api);
    ASSERT_FALSE(headers.cookie.empty());
    const auto status = [&] { return call(api, http::Method::Get, "/api/v1/status", "", "", &headers).status; };
    ASSERT_EQ(status(), 200);

    SimHub &hub = runner.hub();
    ASSERT_EQ(hub.firmware().install(simulatorImage("v9.9.9")), api::FirmwareInstallResult::Accepted);
    hub.advance(api::SimulatedFirmwareUpdate::kWriteMs);
    EXPECT_EQ(hub.firmware().status().state, api::FirmwareUpdateState::Rebooting);
    EXPECT_EQ(status(), 200);  // not until the reboot
    hub.advance(api::SimulatedFirmwareUpdate::kRebootMs);
    EXPECT_EQ(hub.firmware().status().state, api::FirmwareUpdateState::Idle);
    EXPECT_EQ(status(), 401);  // logged out by the restart
}

// A failed write never reboots, so sessions survive it.
TEST(SimApi, FailedFirmwareWriteKeepsSessions) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, false, a.credentials);
    int reboots = 0;
    runner.hub().onFirmwareRebooted = [&] { reboots++; };
    runner.hub().firmware().failWrites = true;
    ASSERT_EQ(runner.hub().firmware().install(simulatorImage("v1.0.0")), api::FirmwareInstallResult::Accepted);
    runner.hub().advance(api::SimulatedFirmwareUpdate::kWriteMs + api::SimulatedFirmwareUpdate::kRebootMs);
    EXPECT_EQ(runner.hub().firmware().status().state, api::FirmwareUpdateState::Failed);
    EXPECT_EQ(reboots, 0);
}

// POST /admin/factory-reset: the simulated restart erases everything, like a
// wiped board, and the hub comes back in first-run setup.
TEST(SimApi, FactoryResetErasesEverythingAfterTheRestart) {
    SimAuth a;
    ASSERT_TRUE(a.secrets.write(api::kHaTokenKey, "fake-ha-token"));
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, /*authEnabled=*/true, a.credentials, nullptr, &a.secrets);
    SimHub &hub = runner.hub();
    const WithCookie headers = setUpAdmin(api);
    ASSERT_FALSE(headers.cookie.empty());
    const auto as = [&](http::Method method, const char *path, const char *query = "") {
        return call(api, method, path, query, "", &headers);
    };
    ASSERT_EQ(hub.remote().setEndpoint({76, 0xE7E7E7E7E7}), api::RemoteResult::Accepted);
    hub.activate("act_watch_apple_tv");
    ASSERT_FALSE(hub.config().devices.empty());
    ASSERT_EQ(call(api, http::Method::Put, "/api/v1/admin/hub", "", R"({"name":"Den"})", &headers).status, 200);
    ASSERT_EQ(hub.hubSettings().identity().name, "Den");

    // Not now: 503, nothing pending.
    EXPECT_EQ(as(http::Method::Post, "/sim/factory-reset", "ready=0").status, 200);
    EXPECT_EQ(as(http::Method::Post, "/api/v1/admin/factory-reset").status, 503);
    EXPECT_EQ(as(http::Method::Post, "/sim/factory-reset", "ready=2").status, 400);
    EXPECT_EQ(as(http::Method::Post, "/sim/factory-reset", "ready=1").status, 200);

    const http::Response accepted = as(http::Method::Post, "/api/v1/admin/factory-reset");
    ASSERT_EQ(accepted.status, 202) << accepted.body;
    hub.advance(api::SimulatedFactoryReset::kRestartMs - 1);
    EXPECT_EQ(a.credentials.state(), auth::CredentialState::Ok);  // not before the restart
    hub.advance(1);
    EXPECT_FALSE(hub.factoryResetPort().pending());

    EXPECT_EQ(a.credentials.state(), auth::CredentialState::Fresh);
    EXPECT_EQ(as(http::Method::Get, "/api/v1/integrations").status, 401);  // the old cookie
    std::string token;
    EXPECT_EQ(a.secrets.read(api::kHaTokenKey, &token), auth::SecretRead::NotFound);
    EXPECT_EQ(hub.haClient().state(), integrations::HaState::NotConfigured);
    EXPECT_FALSE(hub.remote().status().hasEndpoint);
    EXPECT_FALSE(hub.bleHosts().status(SimHub::kBondedDevice).bonded);
    EXPECT_FALSE(hub.engine().hasActiveActivity());
    EXPECT_EQ(hub.hubSettings().identity().name, kSimDefaultName);  // the name goes too
    // The board's first boot, through bootHub(): its defaults (the
    // built-ins and Off bound to system off), saved as generation 1,
    // nothing saved active.
    EXPECT_EQ(hub.lastBoot().config.state, config::ConfigState::Fresh);
    EXPECT_EQ(hub.lastBoot().credentials, auth::CredentialState::Fresh);
    EXPECT_EQ(hub.lastBoot().active, ActiveRestore::NoneSaved);
    EXPECT_EQ(hub.configService().state(), config::ConfigState::Ok);
    EXPECT_EQ(hub.configService().generation(), 1u);
    EXPECT_TRUE(hub.config().devices.empty());
    EXPECT_TRUE(hub.config().activities.empty());
    EXPECT_NE(hub.configService().findIntegration("ha_main"), nullptr);
    ASSERT_EQ(hub.config().globalBindings.size(), 1u);
    EXPECT_EQ(hub.config().globalBindings[0].trigger, "off:tap");
    // Pressing Off on the fresh hub turns an activity off.
    ActivitySpec watch;
    watch.name = "Watch";
    const config::Outcome created = hub.configService().createActivity(watch);
    ASSERT_TRUE(created.ok());
    ASSERT_TRUE(runner.runLine("activate " + created.id)) << runner.error();
    hub.advance(100);
    ASSERT_TRUE(hub.engine().hasActiveActivity());
    hub.tap(remote::Button::Off);
    hub.advance(100);
    EXPECT_FALSE(hub.engine().hasActiveActivity());
    const http::Response again = call(api, http::Method::Post, "/api/v1/auth/setup", "",
                                      R"({"username":"newowner","password":"another horse battery"})");
    EXPECT_EQ(again.status, 201) << again.body;

    // /sim/reset: ready again, nothing pending, and a reset still erases.
    hub.factoryResetPort().ready = false;
    ASSERT_TRUE(runner.runLine("reset"));
    EXPECT_TRUE(hub.factoryResetPort().ready);
    EXPECT_FALSE(hub.factoryResetPort().pending());
    ASSERT_TRUE(hub.factoryResetPort().requestFactoryReset());
    hub.advance(api::SimulatedFactoryReset::kRestartMs);
    EXPECT_TRUE(hub.config().devices.empty());
}

TEST(SimApi, TheStoredTokenConnectsTheFakeHomeAssistant) {
    SimAuth a;
    ASSERT_TRUE(a.secrets.write(api::kHaTokenKey, "fake-ha-token"));
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, false, a.credentials, nullptr, &a.secrets);
    runner.hub().serviceIntegrations();
    EXPECT_EQ(runner.hub().haClient().state(), integrations::HaState::Connected);
    for (const std::string &line : runner.hub().haServer().received()) {
        EXPECT_EQ(line.find("fake-ha-token"), std::string::npos) << line;
    }
    http::Response r = call(api, http::Method::Post, "/sim/ha", "reachable=0");
    EXPECT_EQ(r.status, 200);
    EXPECT_NE(runner.hub().haClient().state(), integrations::HaState::Connected);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/ha", "reachable=1").status, 200);
}

// hub-sim serve's mode: ha_main is the real adapter over the fake HA.
class RealHa : public ::testing::Test {
protected:
    RealHa() {
        a.secrets.write(api::kHaTokenKey, "fake-ha-token");
        runner = std::make_unique<ScenarioRunner>(nullptr, nullptr, true, nullptr, nullptr, &a.credentials, true);
        api = std::make_unique<SimApi>(*runner, false, a.credentials, nullptr, &a.secrets);
        hub().advance(10);
    }
    SimHub &hub() { return runner->hub(); }
    // An HA discovery through the API, run to its end.
    http::Response discover() {
        EXPECT_EQ(call(*api, http::Method::Post, "/api/v1/admin/integrations/ha_main/discover").status, 202);
        http::Response r = call(*api, http::Method::Get, "/api/v1/admin/integrations/ha_main/discover");
        EXPECT_NE(r.body.find("\"done\""), std::string::npos) << r.body;
        return r;
    }
    std::string state(const char *integration) { return SimRuntime(hub()).integrationState(integration); }
    http::Response volume(const char *level = "0.3") {
        const std::string body = std::string(R"({"params":{"level":)") + level + "}}";
        return call(*api, http::Method::Post, "/api/v1/devices/dev_denon/commands/media_player.volume_set/execute", "",
                    body.c_str());
    }

    SimAuth a;
    std::unique_ptr<ScenarioRunner> runner;
    std::unique_ptr<SimApi> api;
};

TEST_F(RealHa, ActionsGoThroughTheClientToTheFakeServer) {
    ASSERT_TRUE(hub().realHomeAssistant());
    ASSERT_EQ(hub().haClient().state(), integrations::HaState::Connected);
    EXPECT_EQ(state("ha_main"), "connected");
    http::Response r = volume();
    EXPECT_EQ(r.status, 200) << r.body;
    EXPECT_NE(r.body.find("\"accepted\""), std::string::npos) << r.body;
    ASSERT_FALSE(hub().haServer().calls().empty());
    EXPECT_EQ(hub().haServer().calls().back(), "media_player.volume_set media_player.denon volume_level=0.3");

    // An activity: its HA steps complete on HA's answers.
    ASSERT_TRUE(runner->runLine("activate act_watch_apple_tv")) << runner->error();
    hub().advance(1000);
    EXPECT_STREQ(hub().engine().activeActivityId(), "act_watch_apple_tv");
    EXPECT_EQ(hub().engine().executor().stats().rejected, 0u);
}

TEST_F(RealHa, DownAndRefusedCallsFailAndUnansweredIsNoReply) {
    // HA refuses: adapter_failed.
    call(*api, http::Method::Post, "/sim/ha", "fail=media_player.volume_set");
    http::Response r = volume();
    EXPECT_NE(r.body.find("adapter_failed"), std::string::npos) << r.body;
    call(*api, http::Method::Post, "/sim/ha", "fail=none");

    // No answer: no reply after 1 s, not a failure (HA may have acted), and
    // never a hang.
    call(*api, http::Method::Post, "/sim/ha", "commands=0");
    r = volume();
    EXPECT_NE(r.body.find("\"status\":\"accepted\""), std::string::npos) << r.body;
    EXPECT_EQ(r.body.find("adapter_timeout"), std::string::npos) << r.body;
    call(*api, http::Method::Post, "/sim/ha", "commands=1");

    // HA down: adapter_unavailable at once, and the integration says so.
    call(*api, http::Method::Post, "/sim/ha", "reachable=0");
    EXPECT_NE(state("ha_main"), "connected");
    const uint32_t before = hub().nowMs();
    r = volume();
    EXPECT_NE(r.body.find("adapter_unavailable"), std::string::npos) << r.body;
    EXPECT_LT(hub().nowMs() - before, 100u);
    const DeviceSpec *denon = hub().configService().findDevice("dev_denon");
    ASSERT_NE(denon, nullptr);
    EXPECT_STREQ(SimRuntime(hub()).deviceAvailability(*denon), "unavailable");
}

TEST_F(RealHa, MuteTogglesThroughTheFakesState) {
    const auto toggle = [&] {
        return call(*api, http::Method::Post, "/api/v1/devices/dev_denon/commands/media_player.mute_toggle/execute", "",
                    R"({"params":{}})");
    };
    // Bad controls: 400.
    EXPECT_EQ(call(*api, http::Method::Post, "/sim/ha", "muted=media_player.denon").status, 400);
    EXPECT_EQ(call(*api, http::Method::Post, "/sim/ha", "muted=media_player.denon:2").status, 400);
    EXPECT_EQ(call(*api, http::Method::Post, "/sim/ha", "muted=light.family_room:1").status, 400);
    EXPECT_EQ(call(*api, http::Method::Post, "/sim/ha", "muted=media_player.nope:1").status, 400);

    // Start muted: the toggle unmutes, then mutes.
    EXPECT_EQ(call(*api, http::Method::Post, "/sim/ha", "muted=media_player.denon:1").status, 200);
    EXPECT_EQ(hub().haServer().muted("media_player.denon"), true);
    http::Response r = toggle();
    EXPECT_NE(r.body.find("\"accepted\""), std::string::npos) << r.body;
    EXPECT_EQ(hub().haServer().calls().back(), "media_player.volume_mute media_player.denon is_volume_muted=false");
    EXPECT_EQ(hub().haServer().muted("media_player.denon"), false);
    r = toggle();
    EXPECT_NE(r.body.find("\"accepted\""), std::string::npos) << r.body;
    EXPECT_EQ(hub().haServer().calls().back(), "media_player.volume_mute media_player.denon is_volume_muted=true");
    EXPECT_EQ(hub().haServer().muted("media_player.denon"), true);
    // /sim/ha reports the latest call; /sim/reset clears it.
    r = call(*api, http::Method::Post, "/sim/ha");
    EXPECT_NE(r.body.find(R"("last_call":{"service":"media_player.volume_mute","entity_id":"media_player.denon",)"
                          R"("data":{"is_volume_muted":true}})"),
              std::string::npos)
        << r.body;
    call(*api, http::Method::Post, "/sim/reset");
    r = call(*api, http::Method::Post, "/sim/ha");
    EXPECT_NE(r.body.find(R"("last_call":null)"), std::string::npos) << r.body;
    EXPECT_EQ(hub().haServer().muted("media_player.denon"), false);
}

TEST_F(RealHa, AnEntityHaForgotIsStale) {
    const DeviceSpec *denon = hub().configService().findDevice("dev_denon");
    ASSERT_NE(denon, nullptr);
    ASSERT_TRUE(denon->haCapabilities.has_value());  // seeded from the fake HA
    EXPECT_STREQ(SimRuntime(hub()).deviceAvailability(*denon), "available");
    call(*api, http::Method::Post, "/sim/ha", "remove=media_player.denon");
    EXPECT_STREQ(SimRuntime(hub()).deviceAvailability(*denon), "available");  // not known until a discovery
    discover();
    EXPECT_STREQ(SimRuntime(hub()).deviceAvailability(*denon), "stale");
    // Refused before HA: real HA would answer success for a missing entity.
    const size_t calls = hub().haServer().calls().size();
    http::Response r = volume();
    EXPECT_NE(r.body.find("entity_not_found"), std::string::npos) << r.body;
    EXPECT_EQ(hub().haServer().calls().size(), calls);
    // A device whose entity HA never had has no commands at all.
    EXPECT_EQ(hub().adapters().at(hub().adapters().find("ha_main"))->commandsFor(sim::haDevice("dev_x", "X", "light", "light.nope")).count,
              0u);
}

TEST_F(RealHa, ADisabledHaMainSaysDisabled) {
    // Disabled, the client has no URL (not_configured); the status says why.
    const std::string body =
        R"({"enabled":false,"revision":)" + std::to_string(hub().configService().findIntegration("ha_main")->revision) + "}";
    http::Response r = call(*api, http::Method::Put, "/api/v1/admin/integrations/ha_main", "", body.c_str());
    ASSERT_EQ(r.status, 200) << r.body;
    hub().advance(10);
    EXPECT_EQ(hub().haClient().state(), integrations::HaState::NotConfigured);
    r = call(*api, http::Method::Get, "/api/v1/status");
    EXPECT_NE(r.body.find(R"({"id":"ha_main","type":"home_assistant","state":"disabled")"), std::string::npos) << r.body;
}

TEST_F(RealHa, VerifyTlsReachesTheTransport) {
    EXPECT_TRUE(hub().haTransport().lastOptions.verifyTls);
    const int before = hub().haTransport().connectAttempts;
    const std::string body = R"({"settings":{"type":"home_assistant","verify_tls":false},"revision":)" +
                             std::to_string(hub().configService().findIntegration("ha_main")->revision) + "}";
    http::Response r = call(*api, http::Method::Put, "/api/v1/admin/integrations/ha_main", "", body.c_str());
    ASSERT_EQ(r.status, 200) << r.body;
    hub().advance(10);
    EXPECT_EQ(hub().haClient().state(), integrations::HaState::Connected);
    EXPECT_FALSE(hub().haTransport().lastOptions.verifyTls);
    EXPECT_GT(hub().haTransport().connectAttempts, before);
}

TEST_F(RealHa, TheFakeHomeAssistantLooksReal) {
    // Discovery: real names, platforms and player attributes.
    const http::Response r = discover();
    EXPECT_NE(r.body.find("\"platform\":\"denonavr\""), std::string::npos) << r.body;
    EXPECT_NE(r.body.find("\"platform\":\"sonos\""), std::string::npos) << r.body;
    EXPECT_NE(r.body.find("Family Room Denon"), std::string::npos) << r.body;
    EXPECT_NE(r.body.find(FakeHaServer::kYamlOnlyEntity), std::string::npos) << r.body;  // pickable
    EXPECT_EQ(hub().haServer().subscriptions(), 0u);

    // Commands, integration services included, per selector kind; a text
    // field skips its service.
    IntegrationAdapter *ha = hub().adapters().at(hub().adapters().find("ha_main"));
    ConfigSpec c;
    c.devices = {sim::haDevice("dev_receiver", "Receiver", "receiver", "media_player.family_room_denon"),
                 sim::haDevice("dev_speaker", "Speaker", "speaker", "media_player.family_room_sonos"),
                 sim::haDevice("dev_tv", "TV", "tv", "media_player.family_room_lg_tv"),
                 sim::haDevice("dev_light", "Light", "light", "light.family_room")};
    addFakeHaSnapshots(&c, hub().haServer());
    // The first param's type; nullopt if the device lacks the command.
    const auto firstParam = [&](const DeviceSpec &d, const char *command) -> std::optional<ParamType> {
        const CommandCatalog catalog = ha->commandsFor(d);
        const int i = catalog.find(command);
        if (i < 0) return std::nullopt;
        EXPECT_GT(catalog.items[i].paramCount, 0) << command;
        return catalog.items[i].paramCount > 0 ? catalog.items[i].params[0].type : ParamType::ActivityRef;
    };
    const auto has = [&](const DeviceSpec &d, const char *command) { return ha->commandsFor(d).find(command) >= 0; };
    EXPECT_EQ(firstParam(c.devices[0], "denonavr.set_dynamic_eq"), ParamType::Bool);
    EXPECT_TRUE(has(c.devices[0], "denonavr.update_audyssey"));
    EXPECT_FALSE(has(c.devices[0], "denonavr.get_command"));  // a text field
    EXPECT_EQ(firstParam(c.devices[0], "media_player.select_source"), ParamType::Enum);
    EXPECT_EQ(firstParam(c.devices[1], "sonos.set_sleep_timer"), ParamType::Int);
    // A field with HA's default, step, unit and translated name and description.
    const auto timer = std::find_if(c.devices[1].haCapabilities->services.begin(),
                                    c.devices[1].haCapabilities->services.end(),
                                    [](const ha::SnapshotService &s) { return s.service == "sonos.set_sleep_timer"; });
    ASSERT_NE(timer, c.devices[1].haCapabilities->services.end());
    const ha::SnapshotParam &sleep = timer->fields.at(0);
    EXPECT_EQ(timer->label, "Set timer");
    EXPECT_EQ(sleep.label, "Sleep Time");
    EXPECT_EQ(sleep.description, "Number of seconds to set the timer.");
    EXPECT_TRUE(sleep.hasDefault);
    EXPECT_EQ(sleep.defaultNumber, 600);
    EXPECT_EQ(sleep.step, 60);
    EXPECT_EQ(sleep.unit, "s");  // HA's "seconds", shortened
    EXPECT_EQ(firstParam(c.devices[1], "sonos.set_option"), ParamType::Bool);
    EXPECT_FALSE(has(c.devices[1], "denonavr.set_dynamic_eq"));  // not its integration
    EXPECT_FALSE(has(c.devices[2], "webostv.select_sound_output"));  // text, as in real HA
    EXPECT_FALSE(has(c.devices[2], "webostv.button"));
    EXPECT_TRUE(has(c.devices[3], "light.turn_on"));
    EXPECT_TRUE(has(c.devices[3], "light.turn_off"));
    EXPECT_EQ(firstParam(c.devices[3], "light.brightness_step"), ParamType::Int);
}

TEST_F(RealHa, EveryDashboardStateCanBeProduced) {
    const auto post = [&](const char *path, const char *query) { return call(*api, http::Method::Post, path, query).status; };
    EXPECT_EQ(state("ha_main"), "connected");
    EXPECT_EQ(state("ble"), "connected");

    EXPECT_EQ(post("/sim/ha", "connect=stall"), 200);
    hub().advance(5000);
    EXPECT_EQ(state("ha_main"), "connecting");
    EXPECT_EQ(post("/sim/ha", "connect=ok"), 200);
    EXPECT_EQ(state("ha_main"), "connected");

    EXPECT_EQ(post("/sim/ha", "auth=reject"), 200);
    hub().advance(5000);
    EXPECT_EQ(state("ha_main"), "auth_failed");
    EXPECT_EQ(post("/sim/ha", "auth=ok"), 200);
    EXPECT_EQ(state("ha_main"), "connected");

    EXPECT_EQ(post("/sim/ha", "reachable=0"), 200);
    EXPECT_EQ(state("ha_main"), "unreachable");

    EXPECT_EQ(post("/sim/ble/radio", "ready=0"), 200);
    EXPECT_EQ(state("ble"), "error");
    EXPECT_EQ(post("/sim/ble/radio", "ready=maybe"), 400);
    // What the dashboard reads.
    const std::string status = call(*api, http::Method::Get, "/api/v1/status").body;
    EXPECT_NE(status.find(R"({"id":"ha_main","type":"home_assistant","state":"unreachable")"), std::string::npos) << status;

    // /sim/reset brings all of it back.
    post("/sim/ha", "auth=reject");
    post("/sim/ha", "connect=stall");
    EXPECT_EQ(post("/sim/reset", ""), 200);
    hub().serviceIntegrations();
    EXPECT_EQ(state("ha_main"), "connected");
    EXPECT_EQ(state("ble"), "connected");
}

namespace {

// A Phase 2 config: only the system and ble integrations, no HA devices.
ConfigSpec phase2Config() {
    ConfigSpec c;
    c.revision = 1;
    c.globalBindingsRevision = 1;
    c.smartButtons.revision = 1;
    c.integrations = {integration("system", "system", "System"), integration("ble", "ble_hid", "Bluetooth")};
    c.devices = {bleDevice("dev_apple_tv", "Apple TV", "streamer")};
    return c;
}

// ha_main exists, isn't configured, and takes its settings.
void expectHaMainUsable(ScenarioRunner &runner, SimApi &api) {
    const IntegrationSpec *ha = runner.hub().configService().findIntegration("ha_main");
    ASSERT_NE(ha, nullptr);
    EXPECT_EQ(ha->type, kHomeAssistantType);
    EXPECT_TRUE(ha->enabled);
    EXPECT_TRUE(ha->url.empty());
    http::Response r = call(api, http::Method::Get, "/api/v1/admin/integrations/ha_main");
    ASSERT_EQ(r.status, 200) << r.body;
    r = call(api, http::Method::Get, "/api/v1/status");
    EXPECT_NE(r.body.find(R"({"id":"ha_main","type":"home_assistant","state":"not_configured")"), std::string::npos)
        << r.body;
    const std::string body = R"({"settings":{"type":"home_assistant","url":"http://ha.local:8123","access_token":")"
                             "fake-ha-token\"},\"revision\":" + std::to_string(ha->revision) + "}";
    r = call(api, http::Method::Put, "/api/v1/admin/integrations/ha_main", "", body.c_str());
    EXPECT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(runner.hub().configService().findIntegration("ha_main")->url, "http://ha.local:8123");
}

}  // namespace

TEST(BuiltinHomeAssistant, AFreshHubHasHaMain) {
    SimAuth a;
    const ConfigSpec seed = phase2Config();
    ScenarioRunner runner(nullptr, nullptr, true, &seed, nullptr, &a.credentials, /*realHomeAssistant=*/true);
    SimApi api(runner, false, a.credentials, nullptr, &a.secrets);
    expectHaMainUsable(runner, api);
}

TEST(BuiltinHomeAssistant, ASavedConfigWithoutItGetsIt) {
    SimAuth a;
    storage::MemoryStorage disk;
    {
        // As Phase 2 firmware saved it.
        storage::SlotRecordStore records(disk, config::kMaxConfigFileBytes);
        uint64_t generation = 0;
        ASSERT_EQ(records.nextGeneration(config::ConfigService::kRecordName, &generation), ErrorCode::Ok);
        ASSERT_EQ(records.save(config::ConfigService::kRecordName, generation, config::serializeConfig(phase2Config())),
                  ErrorCode::Ok);
    }
    ScenarioRunner runner(nullptr, &disk, true, nullptr, nullptr, &a.credentials, /*realHomeAssistant=*/true);
    const config::LoadReport &boot = runner.hub().lastBoot().config;
    ASSERT_EQ(boot.state, config::ConfigState::Ok)
        << errorCodeName(boot.reason) << " " << (boot.issues.ok() ? "" : boot.issues.issues()[0].path);
    EXPECT_EQ(runner.hub().configService().findDevice("dev_apple_tv")->name, "Apple TV");  // the saved one
    SimApi api(runner, false, a.credentials, nullptr, &a.secrets);
    expectHaMainUsable(runner, api);
}

TEST(BuiltinHomeAssistant, ARestoreWithoutItKeepsIt) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, true, nullptr, nullptr, &a.credentials, /*realHomeAssistant=*/true);
    ASSERT_TRUE(runner.hub().load(phase2Config()).ok());
    SimApi api(runner, false, a.credentials, nullptr, &a.secrets);
    expectHaMainUsable(runner, api);
}

TEST_F(RealHa, ResetRestoresTheFakeHomeAssistant) {
    call(*api, http::Method::Post, "/sim/ha", "remove=media_player.denon");
    discover();
    EXPECT_NE(volume().body.find("entity_not_found"), std::string::npos);
    call(*api, http::Method::Post, "/sim/ha", "fail=light.turn_on&commands=0&reachable=0");
    ASSERT_NE(hub().haClient().state(), integrations::HaState::Connected);

    ASSERT_EQ(call(*api, http::Method::Post, "/sim/reset").status, 200);
    FakeHaServer &server = hub().haServer();
    EXPECT_TRUE(server.reachable);
    EXPECT_TRUE(server.answerCommands);
    EXPECT_TRUE(server.failing.empty());
    EXPECT_EQ(server.entities().size(), FakeHaServer().entities().size());
    hub().advance(10);  // reconnects at once, not after the backoff
    EXPECT_EQ(hub().haClient().state(), integrations::HaState::Connected);
    const DeviceSpec *denon = hub().configService().findDevice("dev_denon");
    ASSERT_NE(denon, nullptr);
    EXPECT_STREQ(SimRuntime(hub()).deviceAvailability(*denon), "available");
    const http::Response r = volume();
    EXPECT_NE(r.body.find("\"accepted\""), std::string::npos) << r.body;
}

// MQTT in hub-sim: the fake broker, HA's commands reaching the engine
// through api::runTransition, the integration state, and the /sim/mqtt routes.
TEST(SimApi, MqttDrivesTheHub) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, false, a.credentials);
    SimHub &hub = runner.hub();
    const std::string base = "hubbo/" + hub.mqtt().nodeId() + "/";
    EXPECT_EQ(hub.mqtt().nodeId(), "hubbo_02000000534d");
    EXPECT_EQ(std::string(hub.mqtt().state()), "disabled");

    // mqtt_main set up, and applied as at boot.
    IntegrationSpec mqtt = *hub.configService().findIntegration(kMqttIntegrationId);
    mqtt.enabled = true;
    mqtt.host = "broker.test";
    ASSERT_TRUE(hub.configService().replaceIntegration(kMqttIntegrationId, std::move(mqtt), 1).ok());
    (void)api::applyMqttSettings(hub.configService(), nullptr, hub.mqtt());
    hub.advance(integrations::MqttBridge::kStatesDelayMs + 200);
    EXPECT_TRUE(hub.mqttBroker().connected());
    EXPECT_EQ(hub.mqttBroker().retained().at(base + "activity"), "PowerOff");
    const auto state = [&] {
        JsonDocument doc;
        EXPECT_FALSE(deserializeJson(doc, call(api, http::Method::Get, "/api/v1/integrations").body));
        for (JsonVariantConst i : doc["integrations"].as<JsonArrayConst>()) {
            if (i["id"] == kMqttIntegrationId) return i["state"].as<std::string>();
        }
        return std::string("missing");
    };
    EXPECT_EQ(state(), "connected");

    // HA picks an activity: it runs, and the state follows.
    std::string query = "topic=" + base + "cmd/activity&payload=Play+Xbox";
    ASSERT_EQ(call(api, http::Method::Post, "/sim/mqtt/publish", query.c_str()).status, 200);
    hub.advance(100);
    EXPECT_EQ(hub.engine().activeActivityIdShared(), "act_play_xbox");
    EXPECT_EQ(hub.mqttBroker().retained().at(base + "activity"), "Play Xbox");
    EXPECT_EQ(hub.mqttBroker().retained().at(base + "power"), "ON");

    // A rename republishes the discovery with the new name.
    ASSERT_EQ(call(api, http::Method::Put, "/api/v1/admin/hub", "", R"({"name":"Den"})").status, 200);
    hub.advance(100);
    EXPECT_NE(hub.mqttBroker().retained().at("homeassistant/device/" + hub.mqtt().nodeId() + "/config").find(
                  R"("name":"Den")"),
              std::string::npos);

    // GET /sim/mqtt: what the hub published.
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, call(api, http::Method::Get, "/sim/mqtt").body));
    EXPECT_TRUE(doc["connected"].as<bool>());
    EXPECT_EQ(doc["client_id"], "hubbo_02000000534d");
    EXPECT_EQ(doc["retained"][base + "availability"], "online");
    EXPECT_EQ(doc["subscriptions"][0], base + "cmd/+");
    EXPECT_FALSE(doc["published"].as<JsonArrayConst>().size() == 0);

    // The broker goes away: the will, and the integration says so.
    EXPECT_EQ(call(api, http::Method::Post, "/sim/mqtt", "reachable=2").status, 400);
    EXPECT_EQ(call(api, http::Method::Post, "/sim/mqtt").status, 400);
    ASSERT_EQ(call(api, http::Method::Post, "/sim/mqtt", "reachable=0").status, 200);
    EXPECT_EQ(hub.mqttBroker().retained().at(base + "availability"), "offline");
    EXPECT_EQ(state(), "unreachable");
    ASSERT_EQ(call(api, http::Method::Post, "/sim/mqtt", "reachable=1&auth=reject").status, 200);
    EXPECT_EQ(state(), "auth_failed");
    ASSERT_EQ(call(api, http::Method::Post, "/sim/mqtt", "auth=ok").status, 200);
    EXPECT_EQ(state(), "connected");
}

// The Restart button restarts the hub as POST /admin/restart does.
TEST(SimApi, MqttRestartButton) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, false, a.credentials);
    SimHub &hub = runner.hub();
    integrations::MqttSettings s;
    s.enabled = true;
    s.host = "broker.test";
    hub.mqtt().configure(s);
    hub.advance(200);
    std::string query = "topic=hubbo/" + hub.mqtt().nodeId() + "/cmd/restart&payload=PRESS";
    ASSERT_EQ(call(api, http::Method::Post, "/sim/mqtt/publish", query.c_str()).status, 200);
    hub.advance(api::SimulatedRestart::kRestartMs);
    EXPECT_EQ(hub.restartPort().restarts(), 1);
}
