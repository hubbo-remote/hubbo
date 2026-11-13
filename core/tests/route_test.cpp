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
    EXPECT_EQ(queryValue("a=1&ms=250&b", "ms"), "250");
    EXPECT_EQ(queryValue("a=1&b", "b"), "");
    EXPECT_EQ(queryValue("", "ms"), "");
}

TEST(QueryValue, DecodesOneParameterAtATime) {
    EXPECT_EQ(queryValue("q=office%20lights", "q"), "office lights");
    EXPECT_EQ(queryValue("q=office+lights&limit=5", "q"), "office lights");
    EXPECT_EQ(queryValue("q=%E2%9C%93", "q"), "\xE2\x9C\x93");  // UTF-8 bytes
    // %26 is a literal '&' in the value, never a parameter separator.
    EXPECT_EQ(queryValue("q=a%26limit%3D1&limit=7", "q"), "a&limit=1");
    EXPECT_EQ(queryValue("q=a%26limit%3D1&limit=7", "limit"), "7");
    // Malformed escapes stay as they are.
    EXPECT_EQ(percentDecode("100%"), "100%");
    EXPECT_EQ(percentDecode("%4"), "%4");
    EXPECT_EQ(percentDecode("%zz%41"), "%zzA");
    EXPECT_EQ(queryValue("a=1", "q"), "");
}


TEST_F(RouterTest, ForeignOriginIsCsrfFailed) {
    auto post = [&](const char *path, const char *origin) {
        auth::test::Headers headers;
        headers.items["Host"] = "hubbo.local";
        if (origin != nullptr) headers.items["Origin"] = origin;
        Request r;
        r.method = Method::Post;
        r.path = path;
        r.headers = &headers;
        r.context = as(auth::Principal::admin());
        Response out;
        router.dispatch(r, out);
        return out;
    };
    for (const char *path : {"/api/v1/activities/off", "/sim/reset"}) {
        const Response bad = post(path, "http://evil.example");
        EXPECT_EQ(bad.status, 403) << path;
        EXPECT_NE(bad.body.find("csrf_failed"), std::string::npos) << path;
        EXPECT_EQ(post(path, "http://hubbo.local").status, 200) << path;
        EXPECT_EQ(post(path, nullptr).status, 200) << path << ": no Origin (curl, agents)";
    }
}

TEST_F(RouterTest, BodyTypeComesFromThePolicyTable) {
    router.add("/api/v1/admin/firmware/update", auth::Operation::UpdateFirmware, echo);
    router.add("/api/v1/activities", auth::Operation::CreateActivity, echo);
    auto post = [&](const char *path, const char *type) {
        auth::test::Headers headers;
        headers.items["Content-Type"] = type;
        Request r;
        r.method = Method::Post;
        r.path = path;
        r.body = "\xE9 bytes";
        r.headers = &headers;
        r.context = as(auth::Principal::admin());
        Response out;
        router.dispatch(r, out);
        return out.status;
    };
    EXPECT_EQ(post("/api/v1/admin/firmware/update", "application/octet-stream"), 200);
    EXPECT_EQ(post("/api/v1/admin/firmware/update", "application/json"), 415);
    EXPECT_EQ(post("/api/v1/activities", "application/octet-stream"), 415);
    EXPECT_EQ(post("/api/v1/activities", "application/json"), 200);
    EXPECT_EQ(router.bodyLimit(Method::Post, "/api/v1/admin/firmware/update"), 0x640000u);
}

TEST(StatusLine, NamesEveryStatusARouteSends) {
    EXPECT_STREQ(http::statusLine(200), "200 OK");
    EXPECT_STREQ(http::statusLine(304), "304 Not Modified");
    EXPECT_STREQ(http::statusLine(415), "415 Unsupported Media Type");
    EXPECT_STREQ(http::statusLine(503), "503 Service Unavailable");
    // Any other keeps its own code on the wire, with its class's reason.
    EXPECT_STREQ(http::statusLine(429), "429 Client Error");
    EXPECT_STREQ(http::statusLine(507), "507 Server Error");
    EXPECT_STREQ(http::statusLine(302), "302 Redirection");
}
