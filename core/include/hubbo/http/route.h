#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hubbo/auth/policy.h"

// Transport-independent HTTP routing: the boundary between "an HTTP server"
// (ESP-IDF esp_http_server on the board, a POSIX loopback listener in
// hub-sim) and the API's route handlers. Handlers see only Request/Response
// and never a socket, so the same handlers run in firmware and simulator.
//
// Authorization is the auth module's: the transport asks a
// RequestAuthenticator for the caller's auth::RequestContext before routing,
// and the Router calls auth::authorize() for the route's auth::Operation (so
// the policy table, not each route, decides who may call what). The
// simulator's no-auth authenticator lives only in sim/ behind the host-only
// HUBBO_SIMULATOR_NO_AUTH build flag.
//
// Cold path: allocation is fine here; nothing in this file runs on RF events.
namespace hubbo::http {

using Method = auth::Method;

const char *methodName(Method method);
// False for methods no route uses (the transport answers 405 itself).
bool methodFromName(std::string_view name, Method *out);

class HeaderSource {
public:
    virtual ~HeaderSource() = default;
    // Case-insensitive name; empty view if absent.
    virtual std::string_view header(std::string_view name) const = 0;
};

struct Request {
    Method method = Method::Get;
    std::string_view path;   // decoded path without query, e.g. "/api/v1/status"
    std::string_view query;  // raw query string without '?'
    std::string_view body;
    const HeaderSource *headers = nullptr;
    auth::RequestContext context;  // set by the transport from the authenticator
    // Set by the transport: the request arrived over TLS (cookies then get `Secure`).
    bool secure = false;
};

struct Response {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    void json(int code, std::string text) {
        status = code;
        contentType = "application/json";
        body = std::move(text);
    }
};

// JSON string literal, quotes included, with escaping.
std::string jsonString(std::string_view text);
// Structured error body: {"error":{"code":"...","message":"..."}}.
void jsonError(Response &response, int status, std::string_view code, std::string_view message);
// Value of `key` in a raw query string ("a=1&b=2"), percent-decoded ("+" is
// a space, "%XX" the byte XX; a malformed escape stays as it is), or "" if
// absent. Decodes one parameter, never the whole query, so "%26" can't split
// parameters.
std::string queryValue(std::string_view query, std::string_view key);
// The decoding queryValue() applies ("a+b%20c" -> "a b c").
std::string percentDecode(std::string_view text);

class RequestAuthenticator {
public:
    virtual ~RequestAuthenticator() = default;
    virtual auth::RequestContext authenticate(const Request &request) = 0;
};

// Path parameters captured from "{name}" segments, in order.
struct PathParams {
    static constexpr size_t kMax = 4;
    std::string_view values[kMax];
    size_t count = 0;
    std::string_view operator[](size_t i) const { return i < count ? values[i] : std::string_view(); }
};

class RouteHandler {
public:
    virtual ~RouteHandler() = default;
    virtual void handle(const Request &request, const PathParams &params, Response &response) = 0;
};

class Router {
public:
    // An API route: method and access come from the auth policy table.
    // pattern: literal segments and "{name}" captures, e.g.
    // "/api/v1/activities/{id}/activate". `handler` must outlive the router.
    void add(std::string pattern, auth::Operation operation, RouteHandler &handler);

    // A host-only tool route outside the API (e.g. hub-sim's /sim/*): admin
    // only, Origin-checked, no body checks.
    void addHostRoute(Method method, std::string pattern, RouteHandler &handler);

    // In order: 404 (no path), 405 (path but not method), 403 csrf_failed
    // (unsafe method with a foreign Origin), the policy decision (401/403/
    // 503), and for API operations with a body 413 (over the table's
    // maxBody) and 415 (not application/json, or application/octet-stream
    // for a Body::Binary operation). Then the handler.
    void dispatch(const Request &request, Response &response) const;

    // The largest body the route matching (method, path) accepts, for the
    // transport to check Content-Length before reading the body (and to
    // size the read buffer). 0 for routes that take no body; the firmware
    // upload's is the OTA slot size; auth::maxRequestBody() for unknown
    // routes and host-only tools (dispatch then answers 404/405 or lets the
    // tool decide).
    size_t bodyLimit(Method method, std::string_view path) const;

private:
    static constexpr size_t kMaxSegments = 12;
    struct Segments {
        std::string_view items[kMaxSegments];
        size_t count = 0;
        bool overflow = false;
    };

    struct Route {
        Method method;
        std::vector<std::string> segments;
        auth::Operation operation;  // Count for host-only routes
        RouteHandler *handler;
    };

    static Segments split(std::string_view path);
    static bool admit(const Route &route, const Request &request, Response &response);
    const Route *find(Method method, const Segments &path, PathParams *params, bool *pathMatched) const;
    static bool match(const Route &route, const Segments &path, PathParams *params);
    void addRoute(Route route, std::string_view pattern);
    std::vector<Route> routes_;
};

}  // namespace hubbo::http
