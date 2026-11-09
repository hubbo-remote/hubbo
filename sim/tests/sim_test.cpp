// hub-sim: scenario runner behaviour, simulator routes (without sockets),
// and the loopback / no-auth guard rails.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "fixtures.h"
#include "hubbo/config/json.h"
#include "hubbo/storage/state_store.h"
#include "hubbo/storage/storage.h"
#include "http_server.h"
#include "noauth.h"
#include "scenario.h"
#include "sim_api.h"

using namespace hubbo;
using namespace hubbo::sim;

namespace {

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
                    const char *body = "") {
    static const Headers headers;
    http::Request r;
    r.method = method;
    r.path = path;
    r.query = query;
    r.body = body;
    r.headers = &headers;
    r.context = api.authenticator().authenticate(r);
    http::Response out;
    api.handle(r, out);
    return out;
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

TEST(SimApi, WrongHostIsRefused) {
    SimAuth a;
    ScenarioRunner runner(nullptr, nullptr, /*pinned=*/true, nullptr, nullptr, &a.credentials);
    SimApi api(runner, false, a.credentials);
    http::Request r;
    r.method = http::Method::Get;
    r.path = "/api/v1/status";
    r.context = api.authenticator().authenticate(r);
    http::Response out;
    api.handle(r, out);
    EXPECT_NE(out.status, 200);
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
    const char *create = R"({"name":"Movie","startup":[],"shutdown":[],"bindings":{}})";

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
    void discover() {
        EXPECT_EQ(call(*api, http::Method::Post, "/api/v1/admin/integrations/ha_main/discover").status, 202);
        const http::Response r = call(*api, http::Method::Get, "/api/v1/admin/integrations/ha_main/discover");
        EXPECT_NE(r.body.find("\"done\""), std::string::npos) << r.body;
    }
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
    EXPECT_STREQ(SimRuntime(hub()).integrationState("ha_main"), "connected");
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

TEST_F(RealHa, DownRefusedAndUnansweredCallsFail) {
    // HA refuses: adapter_failed.
    call(*api, http::Method::Post, "/sim/ha", "fail=media_player.volume_set");
    http::Response r = volume();
    EXPECT_NE(r.body.find("adapter_failed"), std::string::npos) << r.body;
    call(*api, http::Method::Post, "/sim/ha", "fail=none");

    // No answer: the action times out (5 s), not hangs.
    call(*api, http::Method::Post, "/sim/ha", "commands=0");
    r = volume();
    EXPECT_NE(r.body.find("\"status\":\"timeout\""), std::string::npos) << r.body;
    EXPECT_NE(r.body.find("adapter_timeout"), std::string::npos) << r.body;
    call(*api, http::Method::Post, "/sim/ha", "commands=1");

    // HA down: adapter_unavailable at once, and the integration says so.
    call(*api, http::Method::Post, "/sim/ha", "reachable=0");
    EXPECT_STRNE(SimRuntime(hub()).integrationState("ha_main"), "connected");
    const uint32_t before = hub().nowMs();
    r = volume();
    EXPECT_NE(r.body.find("adapter_unavailable"), std::string::npos) << r.body;
    EXPECT_LT(hub().nowMs() - before, 100u);
    const DeviceSpec *denon = hub().configService().findDevice("dev_denon");
    ASSERT_NE(denon, nullptr);
    EXPECT_STREQ(SimRuntime(hub()).deviceAvailability(*denon), "unavailable");
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

namespace {

// A Phase 2 config: only the system and ble integrations, no HA devices.
ConfigSpec phase2Config() {
    ConfigSpec c;
    c.revision = 1;
    c.globalBindingsRevision = 1;
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
