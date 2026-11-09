// Transport-independent router: matching, path params, method and the auth
// policy decision per route.
#include <gtest/gtest.h>

#include "auth_test_util.h"
#include "hubbo/http/route.h"

using namespace hubbo;
using namespace hubbo::http;

namespace {

struct Echo : RouteHandler {
    void handle(const Request &r, const PathParams &p, Response &out) override {
        std::string body = methodName(r.method);
        for (size_t i = 0; i < p.count; i++) body += " " + std::string(p[i]);
        out.json(200, body);
    }
};

auth::RequestContext as(auth::Principal who) {
    auth::RequestContext c;
    c.principal = who;
    c.csrfValid = true;
    return c;
}

class RouterTest : public ::testing::Test {
protected:
    RouterTest() {
        router.add("/api/v1/session", auth::Operation::GetSession, echo);
        router.add("/api/v1/activities/{id}/activate", auth::Operation::ActivateActivity, echo);
        router.add("/api/v1/activities/off", auth::Operation::ActivitiesOff, echo);
        router.add("/api/v1/admin/devices/{id}", auth::Operation::DeleteDevice, echo);
        router.addHostRoute(Method::Post, "/sim/reset", echo);
    }
    Response call(Method m, const char *path, auth::Principal who = auth::Principal::admin()) {
        Request r;
        r.method = m;
        r.path = path;
        r.context = as(who);
        Response out;
        router.dispatch(r, out);
        return out;
    }
    Echo echo;
    Router router;
};

}  // namespace

TEST_F(RouterTest, MatchesLiteralsAndCaptures) {
    EXPECT_EQ(call(Method::Get, "/api/v1/session").body, "GET");
    EXPECT_EQ(call(Method::Post, "/api/v1/activities/act_watch_apple_tv/activate").body,
              "POST act_watch_apple_tv");
    EXPECT_EQ(call(Method::Post, "/api/v1/activities/off").body, "POST");
    EXPECT_EQ(call(Method::Get, "/api/v1/session/").status, 200);  // trailing slash tolerated
}

TEST_F(RouterTest, NotFoundAndMethodNotAllowed) {
    EXPECT_EQ(call(Method::Get, "/api/v1/nope").status, 404);
    EXPECT_EQ(call(Method::Get, "/api/v1/activities/x/activate/more").status, 404);
    EXPECT_EQ(call(Method::Get, "/a/b/c/d/e/f/g/h/i/j/k/l/m/n").status, 404);  // too deep
    const Response r = call(Method::Post, "/api/v1/session");
    EXPECT_EQ(r.status, 405);
    EXPECT_NE(r.body.find("\"method_not_allowed\""), std::string::npos);
}

TEST_F(RouterTest, AuthPolicyDecides) {
    EXPECT_EQ(call(Method::Get, "/api/v1/session", auth::Principal::anonymous()).status, 200);
    const Response anon = call(Method::Post, "/api/v1/activities/off", auth::Principal::anonymous());
    EXPECT_EQ(anon.status, 401);
    EXPECT_NE(anon.body.find("\"unauthenticated\""), std::string::npos);
    EXPECT_EQ(call(Method::Post, "/api/v1/activities/off", auth::Principal::token()).status, 200);
    EXPECT_EQ(call(Method::Delete, "/api/v1/admin/devices/dev_1", auth::Principal::token()).status, 403);
    EXPECT_EQ(call(Method::Delete, "/api/v1/admin/devices/dev_1", auth::Principal::admin()).status, 200);
}

TEST_F(RouterTest, HostRoutesNeedAdmin) {
    EXPECT_EQ(call(Method::Post, "/sim/reset", auth::Principal::anonymous()).status, 401);
    EXPECT_EQ(call(Method::Post, "/sim/reset", auth::Principal::token()).status, 403);
    EXPECT_EQ(call(Method::Post, "/sim/reset", auth::Principal::admin()).status, 200);
}

TEST_F(RouterTest, BodyLimitComesFromThePolicyTable) {
    router.add("/api/v1/activities", auth::Operation::CreateActivity, echo);
    router.add("/api/v1/admin/restore", auth::Operation::RestoreBackup, echo);
    EXPECT_EQ(router.bodyLimit(Method::Post, "/api/v1/activities"),
              auth::operationInfo(auth::Operation::CreateActivity).maxBody);
    EXPECT_EQ(router.bodyLimit(Method::Post, "/api/v1/admin/restore"), auth::maxRequestBody());
    EXPECT_EQ(router.bodyLimit(Method::Post, "/api/v1/activities/off"), 0u) << "takes no body";
    EXPECT_EQ(router.bodyLimit(Method::Post, "/api/v1/nope"), auth::maxRequestBody()) << "404 decided later";
    EXPECT_EQ(router.bodyLimit(Method::Post, "/sim/reset"), auth::maxRequestBody()) << "host tools decide";
}

TEST(Http, Helpers) {
    EXPECT_EQ(jsonString("a\"b\\c\n\x01"), "\"a\\\"b\\\\c\\n\\u0001\"");
    Method m;
    EXPECT_TRUE(methodFromName("PUT", &m));
    EXPECT_EQ(m, Method::Put);
    EXPECT_FALSE(methodFromName("BREW", &m));
    EXPECT_EQ(queryParam("a=1&ms=250&b", "ms"), "250");
    EXPECT_EQ(queryParam("a=1&b", "b"), "");
    EXPECT_EQ(queryParam("", "ms"), "");
}


TEST_F(RouterTest, HostAllowlistBlocksRebinding) {
    router.setAllowedHosts({"localhost", "127.0.0.1", "[::1]", "hubbo.local"});
    auto withHost = [&](const char *host) {
        auth::test::Headers headers;
        if (host != nullptr) headers.items["Host"] = host;
        Request r;
        r.method = Method::Get;
        r.path = "/api/v1/session";
        r.headers = &headers;
        r.context = as(auth::Principal::admin());
        Response out;
        router.dispatch(r, out);
        return out;
    };
    for (const char *ok : {"localhost", "localhost:8080", "127.0.0.1:8080", "[::1]:8080", "[::1]", "HUBBO.local"}) {
        EXPECT_EQ(withHost(ok).status, 200) << ok;
    }
    for (const char *bad : {"evil.example", "evil.example:8080", "localhost.evil.example", "", "[::2]:80"}) {
        const Response r = withHost(bad);
        EXPECT_EQ(r.status, 421) << bad;
        EXPECT_NE(r.body.find("host_not_allowed"), std::string::npos);
    }
    EXPECT_EQ(withHost(nullptr).status, 421) << "no Host header";
}

TEST_F(RouterTest, HostRoutesUseThePolicyTableIncludingCsrf) {
    Request r;
    r.method = Method::Post;
    r.path = "/sim/reset";
    r.context.principal = auth::Principal::admin();
    r.context.csrfValid = false;
    Response out;
    router.dispatch(r, out);
    EXPECT_EQ(out.status, 403);
    EXPECT_NE(out.body.find("csrf_failed"), std::string::npos);
}
