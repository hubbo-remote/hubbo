// hub-sim: scenario runner behaviour, simulator routes (without sockets),
// and the loopback / no-auth guard rails.
#include <gtest/gtest.h>

#include <sstream>

#include "http_server.h"
#include "noauth.h"
#include "scenario.h"
#include "sim_api.h"

using namespace hubbo;
using namespace hubbo::sim;

namespace {

bool runScript(ScenarioRunner &runner, const std::string &text) {
    std::istringstream in(text);
    return runner.run(in, "test");
}

http::Response call(SimApi &api, http::RequestAuthenticator &auth, http::Method method, const char *path,
                    const char *query = "", const char *body = "") {
    http::Request r;
    r.method = method;
    r.path = path;
    r.query = query;
    r.body = body;
    r.context = auth.authenticate(r);
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
    ScenarioRunner runner;
    SimApi api(runner, "simulator_no_auth");
    SimulatorNoAuth noAuth;

    http::Response r = call(api, noAuth, http::Method::Post, "/api/v1/activities/act_watch_apple_tv/activate");
    EXPECT_EQ(r.status, 202);
    r = call(api, noAuth, http::Method::Get, "/api/v1/status");
    EXPECT_EQ(r.status, 200);
    EXPECT_NE(r.body.find("\"active_activity_id\":\"act_watch_apple_tv\""), std::string::npos);
    EXPECT_NE(r.body.find("\"auth_mode\":\"simulator_no_auth\""), std::string::npos);

    EXPECT_EQ(call(api, noAuth, http::Method::Post, "/api/v1/activities/nope/activate").status, 404);
    EXPECT_EQ(call(api, noAuth, http::Method::Post, "/sim/remote/up/tap").status, 200);
    EXPECT_EQ(call(api, noAuth, http::Method::Post, "/sim/remote/up/smash").status, 400);
    EXPECT_EQ(call(api, noAuth, http::Method::Post, "/sim/advance", "ms=250").status, 200);
    r = call(api, noAuth, http::Method::Get, "/sim/log");
    EXPECT_NE(r.body.find("key_down key=ARROW_UP"), std::string::npos);

    r = call(api, noAuth, http::Method::Post, "/sim/script", "", "drain\noff\nexpect ha_main dev_lg_tv media_player.turn_off\n");
    EXPECT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(call(api, noAuth, http::Method::Post, "/api/v1/activities/off").status, 409);
}

TEST(SimApi, AuthEnabledModeRefusesProtectedRoutes) {
    ScenarioRunner runner;
    SimApi api(runner, "enforced");
    DenyAllAuth auth;
    const http::Response status = call(api, auth, http::Method::Get, "/api/v1/status");
    EXPECT_EQ(status.status, 401);
    EXPECT_NE(status.body.find("\"unauthenticated\""), std::string::npos);
    EXPECT_EQ(call(api, auth, http::Method::Post, "/api/v1/activities/act_watch_apple_tv/activate").status, 401);
    EXPECT_EQ(call(api, auth, http::Method::Post, "/sim/reset").status, 401);
    EXPECT_FALSE(runner.hub().engine().hasActiveActivity());
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
