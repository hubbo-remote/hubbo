#include "hubbo/http/route.h"

#include <cstdio>

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
    addRoute({auth::operationInfo(operation).method, {}, false, operation, &handler}, pattern);
}

void Router::addHostRoute(Method method, std::string pattern, RouteHandler &handler) {
    addRoute({method, {}, true, auth::Operation::Count, &handler}, pattern);
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

void Router::dispatch(const Request &request, Response &response) const {
    const Segments path = split(request.path);
    bool pathMatched = false;
    for (const auto &route : routes_) {
        PathParams params;
        if (!match(route, path, &params)) continue;
        pathMatched = true;
        if (route.method != request.method) continue;

        auth::Decision decision;
        if (!route.hostOnly) {
            decision = auth::authorize(route.operation, request.context);
        } else if (request.context.principal.role == auth::Role::Admin) {
            decision = auth::Decision::Allow;
        } else {
            decision = request.context.principal.role == auth::Role::Anonymous ? auth::Decision::Unauthenticated
                                                                                : auth::Decision::Forbidden;
        }
        if (decision != auth::Decision::Allow) {
            jsonError(response, auth::httpStatus(decision), auth::decisionCode(decision), "not allowed");
            return;
        }
        route.handler->handle(request, params, response);
        return;
    }
    if (pathMatched) {
        jsonError(response, 405, "method_not_allowed", "method not allowed");
    } else {
        jsonError(response, 404, "not_found", "no such route");
    }
}

}  // namespace hubbo::http
