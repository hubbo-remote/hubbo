#include "hubbo/http/route.h"

#include <cstdio>

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

const char *statusLine(int status) {
    switch (status) {
        case 200: return "200 OK";
        case 201: return "201 Created";
        case 202: return "202 Accepted";
        case 204: return "204 No Content";
        case 304: return "304 Not Modified";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 409: return "409 Conflict";
        case 411: return "411 Length Required";
        case 412: return "412 Precondition Failed";
        case 413: return "413 Payload Too Large";
        case 415: return "415 Unsupported Media Type";
        case 422: return "422 Unprocessable Entity";
        case 501: return "501 Not Implemented";
        case 503: return "503 Service Unavailable";
        default: break;
    }
    // Any other: its own code (never rewritten), with its class's reason.
    thread_local char line[32];
    const char *reason = "Informational";
    if (status >= 500) reason = "Server Error";
    else if (status >= 400) reason = "Client Error";
    else if (status >= 300) reason = "Redirection";
    else if (status >= 200) reason = "Success";
    std::snprintf(line, sizeof(line), "%d %s", status, reason);
    return line;
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

void jsonError(Response &response, int status, std::string_view code, std::string_view message,
               std::string_view docs) {
    std::string body = "{\"error\":{\"code\":" + jsonString(code) + ",\"message\":" + jsonString(message);
    if (!docs.empty()) body += ",\"docs\":" + jsonString(docs);
    body += "}}";
    response.json(status, std::move(body));
}

namespace {

// The raw slice of `key`'s value (queryValue decodes it).
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

}  // namespace

std::string percentDecode(std::string_view text) {
    auto hex = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            out += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

std::string queryValue(std::string_view query, std::string_view key) {
    return percentDecode(queryParam(query, key));
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
    const auth::OperationInfo &info = auth::operationInfo(operation);
    addRoute({info.method, {}, operation, &handler, info.requirement}, pattern);
}

void Router::addHostRoute(Method method, std::string pattern, RouteHandler &handler, auth::Requirement access) {
    addRoute({method, {}, auth::Operation::Count, &handler, access}, pattern);
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

void Router::pointErrorsTo(std::string prefix, std::string docs) {
    docsPrefix_ = std::move(prefix);
    docs_ = std::move(docs);
}

bool Router::admit(const Route &route, const Request &request, Response &response) const {
    const bool api = route.operation != auth::Operation::Count;
    const std::string_view origin = request.headers ? request.headers->header("Origin") : std::string_view();
    const std::string_view host = request.headers ? request.headers->header("Host") : std::string_view();
    if (auth::isUnsafe(request.method) && !auth::originAllowed(origin, host)) {
        jsonError(response, 403, "csrf_failed", "Origin does not match this hub");
        return false;
    }
    const auth::Decision decision = auth::authorize(route.access, request.context);
    if (decision != auth::Decision::Allow) {
        jsonError(response, auth::httpStatus(decision), auth::decisionCode(decision), auth::decisionMessage(decision),
                  decision == auth::Decision::Unauthenticated ? std::string_view(docs_) : std::string_view());
        return false;
    }
    // Bodies of API operations: size from the policy table, then type.
    if (!api || request.body.empty()) return true;
    const auth::OperationInfo &info = auth::operationInfo(route.operation);
    if (info.maxBody > 0) {
        if (request.body.size() > info.maxBody) {
            jsonError(response, 413, "payload_too_large", "Body exceeds " + std::to_string(info.maxBody) + " bytes");
            return false;
        }
        const std::string_view type = request.headers ? request.headers->header("Content-Type") : std::string_view();
        const char *want = auth::mediaType(info.body);
        if (!auth::isMediaType(type, want)) {
            jsonError(response, 415, "unsupported_media_type", std::string("Send Content-Type: ") + want);
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
    PathParams params;
    bool pathMatched = false;
    const Segments path = split(request.path);
    const Route *route = find(request.method, path, &params, &pathMatched);
    if (route != nullptr) {
        if (admit(*route, request, response)) route->handler->handle(request, params, response);
        return;
    }
    if (pathMatched) {
        jsonError(response, 405, "method_not_allowed", "method not allowed");
    } else {
        const bool hinted = path.count >= 1 && path.items[0] == docsPrefix_;
        jsonError(response, 404, "not_found", "no such route", hinted ? std::string_view(docs_) : std::string_view());
    }
}

}  // namespace hubbo::http
