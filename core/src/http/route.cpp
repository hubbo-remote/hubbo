#include "hubbo/http/route.h"

#include <cstdio>

#include "hubbo/auth/crypto.h"
#include "hubbo/auth/csrf.h"

namespace hubbo::http {

const char *methodName(Method method) {
    switch (method) {
        case Method::Get: return "GET";
        case Method::Post: return "POST";
        case Method::Put: return "PUT";
        case Method::Delete: return "DELETE";
    }
    return "GET";
}

bool methodFromName(std::string_view name, Method *out) {
    for (Method m : {Method::Get, Method::Post, Method::Put, Method::Delete}) {
        if (name == methodName(m)) {
            *out = m;
            return true;
        }
    }
    return false;
}

std::string jsonString(std::string_view text) {
    std::string out = "\"";
    out.reserve(text.size() + 2);
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

void jsonError(Response &response, int status, std::string_view code, std::string_view message) {
    response.json(status, "{\"error\":{\"code\":" + jsonString(code) + ",\"message\":" + jsonString(message) + "}}");
}

std::string_view queryParam(std::string_view query, std::string_view key) {
    while (!query.empty()) {
        const size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        const size_t eq = pair.find('=');
        if (eq != std::string_view::npos && pair.substr(0, eq) == key) return pair.substr(eq + 1);
        if (amp == std::string_view::npos) break;
        query.remove_prefix(amp + 1);
    }
    return {};
}

Router::Segments Router::split(std::string_view path) {
    Segments out;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        if (slash == std::string_view::npos) slash = path.size();
        if (slash > start) {
            if (out.count == kMaxSegments) {
                out.overflow = true;
                break;
            }
            out.items[out.count++] = path.substr(start, slash - start);
        }
        start = slash + 1;
    }
    return out;
}

void Router::addRoute(Route route, std::string_view pattern) {
    const Segments segments = split(pattern);
    for (size_t i = 0; i < segments.count; i++) route.segments.emplace_back(segments.items[i]);
    routes_.push_back(std::move(route));
}

void Router::add(std::string pattern, auth::Operation operation, RouteHandler &handler) {
    addRoute({auth::operationInfo(operation).method, {}, operation, &handler}, pattern);
}

void Router::addHostRoute(Method method, std::string pattern, RouteHandler &handler) {
    addRoute({method, {}, auth::Operation::Count, &handler}, pattern);
}

bool Router::hostAllowed(std::string_view host) const {
    if (allowedHosts_.empty()) return true;
    // Strip the port: "name:8080" or "[::1]:8080".
    if (!host.empty() && host.front() == '[') {
        const size_t end = host.find(']');
        if (end != std::string_view::npos) host = host.substr(0, end + 1);
    } else {
        host = host.substr(0, host.find(':'));
    }
    for (const auto &allowed : allowedHosts_) {
        if (auth::asciiEqualsIgnoreCase(host, allowed)) return true;
    }
    return false;
}

bool Router::match(const Route &route, const Segments &path, PathParams *params) {
    if (path.overflow || path.count != route.segments.size()) return false;
    PathParams captured;
    for (size_t i = 0; i < path.count; i++) {
        const std::string &want = route.segments[i];
        if (want.size() >= 2 && want.front() == '{' && want.back() == '}') {
            if (captured.count == PathParams::kMax) return false;
            captured.values[captured.count++] = path.items[i];
        } else if (want != path.items[i]) {
            return false;
        }
    }
    *params = captured;
    return true;
}

bool Router::admit(const Route &route, const Request &request, Response &response) {
    const bool api = route.operation != auth::Operation::Count;
    const std::string_view origin = request.headers ? request.headers->header("Origin") : std::string_view();
    const std::string_view host = request.headers ? request.headers->header("Host") : std::string_view();
    if (auth::isUnsafe(request.method) && !auth::originAllowed(origin, host)) {
        jsonError(response, 403, "forbidden", "Origin does not match this hub");
        return false;
    }
    const auth::Decision decision =
        api ? auth::authorize(route.operation, request.context)
            : auth::authorize(auth::Requirement::AdminOnly, auth::Csrf::Check, route.method, request.context);
    if (decision != auth::Decision::Allow) {
        jsonError(response, auth::httpStatus(decision), auth::decisionCode(decision), auth::decisionMessage(decision));
        return false;
    }
    // JSON bodies of API operations: size from the policy table, then type.
    const uint32_t maxBody = api ? auth::operationInfo(route.operation).maxBody : 0;
    if (maxBody > 0 && !request.body.empty()) {
        if (request.body.size() > maxBody) {
            jsonError(response, 413, "payload_too_large", "Body exceeds " + std::to_string(maxBody) + " bytes");
            return false;
        }
        if (!auth::isJsonContentType(request.headers ? request.headers->header("Content-Type") : std::string_view())) {
            jsonError(response, 415, "unsupported_media_type", "Send Content-Type: application/json");
            return false;
        }
    }
    return true;
}

const Router::Route *Router::find(Method method, const Segments &path, PathParams *params,
                                  bool *pathMatched) const {
    *pathMatched = false;
    for (const auto &route : routes_) {
        if (!match(route, path, params)) continue;
        *pathMatched = true;
        if (route.method == method) return &route;
    }
    return nullptr;
}

size_t Router::bodyLimit(Method method, std::string_view path) const {
    PathParams params;
    bool pathMatched = false;
    const Route *route = find(method, split(path), &params, &pathMatched);
    if (route == nullptr || route->operation == auth::Operation::Count) return auth::maxRequestBody();
    return auth::operationInfo(route->operation).maxBody;
}

void Router::dispatch(const Request &request, Response &response) const {
    if (!allowedHosts_.empty()) {
        const std::string_view host = request.headers ? request.headers->header("Host") : std::string_view();
        if (host.empty() || !hostAllowed(host)) {
            jsonError(response, 421, "host_not_allowed", "Host header is not one of this hub's names");
            return;
        }
    }
    PathParams params;
    bool pathMatched = false;
    const Route *route = find(request.method, split(request.path), &params, &pathMatched);
    if (route != nullptr) {
        if (admit(*route, request, response)) route->handler->handle(request, params, response);
        return;
    }
    if (pathMatched) {
        jsonError(response, 405, "method_not_allowed", "method not allowed");
    } else {
        jsonError(response, 404, "not_found", "no such route");
    }
}

}  // namespace hubbo::http
