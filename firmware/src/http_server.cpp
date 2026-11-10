#include "http_server.h"

#include "hubbo/auth/policy.h"
#include "console.h"
#include "web_assets.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_timer.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <deque>
#include <string>
#include <utility>

namespace hubbo::fw {

namespace {

constexpr int kRecvTimeoutRetries = 3;

class EspHeaders : public http::HeaderSource {
public:
    explicit EspHeaders(httpd_req_t *req) : req_(req) {}

    std::string_view header(std::string_view name) const override {
        const std::string key(name);
        const size_t length = httpd_req_get_hdr_value_len(req_, key.c_str());  // case-insensitive
        if (length == 0) {
            return {};
        }
        // A deque keeps earlier strings in place, so returned views stay valid
        // for the whole request.
        std::string &value = cache_.emplace_back(length + 1, '\0');
        if (httpd_req_get_hdr_value_str(req_, key.c_str(), value.data(), value.size()) != ESP_OK) {
            value.clear();
            return {};
        }
        value.resize(length);
        return value;
    }

private:
    httpd_req_t *req_;
    mutable std::deque<std::string> cache_;
};

bool methodOf(int method, http::Method *out) {
    switch (method) {
        case HTTP_GET: *out = http::Method::Get; return true;
        case HTTP_POST: *out = http::Method::Post; return true;
        case HTTP_PUT: *out = http::Method::Put; return true;
        case HTTP_DELETE: *out = http::Method::Delete; return true;
        default: return false;
    }
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Percent-decodes a path. False on a malformed escape or an encoded NUL or
// '/' (which would change how the path splits into segments).
bool decodePath(std::string_view raw, std::string *out) {
    out->clear();
    out->reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] != '%') {
            out->push_back(raw[i]);
            continue;
        }
        if (i + 2 >= raw.size()) return false;
        const int hi = hexValue(raw[i + 1]);
        const int lo = hexValue(raw[i + 2]);
        if (hi < 0 || lo < 0) return false;
        const char c = static_cast<char>(hi * 16 + lo);
        if (c == '\0' || c == '/') return false;
        out->push_back(c);
        i += 2;
    }
    return true;
}

const char *statusLine(int status) {
    switch (status) {
        case 200: return "200 OK";
        case 201: return "201 Created";
        case 202: return "202 Accepted";
        case 204: return "204 No Content";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 409: return "409 Conflict";
        case 412: return "412 Precondition Failed";
        case 413: return "413 Payload Too Large";
        case 415: return "415 Unsupported Media Type";
        case 421: return "421 Misdirected Request";
        case 422: return "422 Unprocessable Entity";
        case 429: return "429 Too Many Requests";
        case 501: return "501 Not Implemented";
        case 503: return "503 Service Unavailable";
        default: return "500 Internal Server Error";
    }
}

esp_err_t send(httpd_req_t *req, const http::Response &response) {
    httpd_resp_set_status(req, statusLine(response.status));
    httpd_resp_set_type(req, response.contentType.c_str());
    // Every header string lives in `response` until httpd_resp_send returns.
    for (const auto &header : response.headers) {
        httpd_resp_set_hdr(req, header.first.c_str(), header.second.c_str());
    }
    return httpd_resp_send(req, response.body.data(), static_cast<ssize_t>(response.body.size()));
}

// The peer's IPv4 address, or its IPv6 /64 prefix; 0 if unknown.
uint64_t clientKey(httpd_req_t *req) {
    sockaddr_storage addr = {};
    socklen_t length = sizeof(addr);
    if (getpeername(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr *>(&addr), &length) != 0) {
        return 0;
    }
    if (addr.ss_family == AF_INET) {
        return ntohl(reinterpret_cast<const sockaddr_in &>(addr).sin_addr.s_addr);
    }
#if LWIP_IPV6
    if (addr.ss_family == AF_INET6) {
        const uint8_t *bytes = reinterpret_cast<const sockaddr_in6 &>(addr).sin6_addr.s6_addr;
        static const uint8_t kV4Mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (memcmp(bytes, kV4Mapped, sizeof(kV4Mapped)) == 0) {
            return (uint32_t{bytes[12]} << 24) | (uint32_t{bytes[13]} << 16) | (uint32_t{bytes[14]} << 8) |
                   bytes[15];
        }
        uint64_t prefix = 0;
        for (int i = 0; i < 8; i++) {
            prefix = (prefix << 8) | bytes[i];
        }
        return prefix;
    }
#endif
    return 0;
}

esp_err_t sendError(httpd_req_t *req, int status, std::string_view code, std::string_view message) {
    http::Response response;
    http::jsonError(response, status, code, message);
    return send(req, response);
}

}  // namespace

struct HttpServerAccess {
    static esp_err_t onRequest(httpd_req_t *req) {
        return static_cast<HttpServer *>(req->user_ctx)->serve(req);
    }
    static esp_err_t onStatic(httpd_req_t *req) {
        return static_cast<HttpServer *>(req->user_ctx)->serveStatic(req);
    }
};

bool HttpServer::serveWebAdmin() {
    if (handle_ == nullptr) {
        return false;
    }
    httpd_uri_t uri = {};
    uri.uri = "/*";
    uri.method = HTTP_GET;
    uri.handler = &HttpServerAccess::onStatic;
    uri.user_ctx = this;
    return httpd_register_uri_handler(static_cast<httpd_handle_t>(handle_), &uri) == ESP_OK;
}

int HttpServer::serveStatic(void *raw) {
    auto *req = static_cast<httpd_req_t *>(raw);
    requests_ = requests_ + 1;

    const EspHeaders headers(req);
    if (!router_->hostAllowed(headers.header("Host"))) {
        return sendError(req, 421, "host_not_allowed", "Host header is not one of this hub's names");
    }
    std::string_view path(req->uri);
    path = path.substr(0, path.find('?'));
    if (path == "/") {
        path = "/index.html";
    }
    const web::Asset *asset = web::find(path);
    if (asset == nullptr) {
        // An SPA route (a page reload on /devices/...) gets the app; a
        // missing file or hashed asset doesn't. (/api/* and /debug/* never
        // get here: their handlers are registered first.)
        const bool wantsHtml = headers.header("Accept").find("text/html") != std::string_view::npos;
        if (!wantsHtml || path.substr(0, 3) == "/a/") {
            return sendError(req, 404, "not_found", "Not found");
        }
        asset = web::find("/index.html");
    }

    if (asset->immutable) {
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    } else {
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        httpd_resp_set_hdr(req, "ETag", web::kEtag);
        if (headers.header("If-None-Match") == web::kEtag) {
            httpd_resp_set_status(req, "304 Not Modified");
            return httpd_resp_send(req, nullptr, 0);
        }
    }
    httpd_resp_set_type(req, asset->contentType);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy", web::kCsp);  // only matters on index.html
    // From flash: lwIP copies as it sends, nothing is staged in RAM.
    return httpd_resp_send(req, reinterpret_cast<const char *>(asset->gzip), static_cast<ssize_t>(asset->size));
}

bool HttpServer::begin(const http::Router &router, http::RequestAuthenticator *authenticator) {
    if (handle_ != nullptr) {
        return true;
    }
    router_ = &router;
    authenticator_ = authenticator;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kPort;
    config.core_id = kCore;
    config.task_priority = kPriority;
    config.stack_size = kStackBytes;
    // A browser's cold load opens about 6 connections; with 4, LRU purging
    // dropped some mid-load. lwIP has 16 sockets: httpd adds 3 internally,
    // plus the HA WebSocket, OTA and mDNS.
    config.max_open_sockets = 7;
    config.max_uri_handlers = 12;  // the API (one per method), the dev console and the web admin
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        con.println("http: server failed to start");
        return false;
    }
    for (const httpd_method_t method : {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE}) {
        httpd_uri_t uri = {};
        uri.uri = "/api/*";
        uri.method = method;
        uri.handler = &HttpServerAccess::onRequest;
        uri.user_ctx = this;
        httpd_register_uri_handler(server, &uri);
    }
    handle_ = server;
    con.printf("http: listening on port %u (core %d, priority %u)\n", kPort, kCore, kPriority);
    return true;
}

bool HttpServer::post(std::function<void()> fn) {
    if (handle_ == nullptr) {
        return false;
    }
    auto *work = new std::function<void()>(std::move(fn));
    const auto run = [](void *arg) {
        auto *f = static_cast<std::function<void()> *>(arg);
        (*f)();
        delete f;
    };
    if (httpd_queue_work(static_cast<httpd_handle_t>(handle_), run, work) != ESP_OK) {
        delete work;
        return false;
    }
    return true;
}

#ifdef HUBBO_DEV_CONSOLE
bool HttpServer::addDebugGet(const char *uri, DebugGet handler) {
    if (handle_ == nullptr) {
        return false;
    }
    auto owned = std::make_unique<DebugGet>(std::move(handler));
    httpd_uri_t route = {};
    route.uri = uri;
    route.method = HTTP_GET;
    route.user_ctx = owned.get();
    route.handler = [](httpd_req_t *req) -> esp_err_t {
        const std::string_view full(req->uri);
        const size_t queryAt = full.find('?');
        const std::string_view query = queryAt == std::string_view::npos ? std::string_view() : full.substr(queryAt + 1);
        const std::string body = (*static_cast<DebugGet *>(req->user_ctx))(query);
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, body.data(), static_cast<ssize_t>(body.size()));
    };
    if (httpd_register_uri_handler(static_cast<httpd_handle_t>(handle_), &route) != ESP_OK) {
        return false;
    }
    debugGets_.push_back(std::move(owned));
    return true;
}
#endif

bool HttpServer::every(uint32_t periodMs, std::function<void()> fn, std::function<bool()> due) {
    auto job = std::make_unique<Periodic>();
    job->server = this;
    job->fn = std::move(fn);
    job->due = std::move(due);

    esp_timer_create_args_t args = {};
    args.name = "httpd_every";
    args.arg = job.get();
    args.callback = [](void *arg) {  // esp_timer task: only decide and queue
        auto *p = static_cast<Periodic *>(arg);
        if (p->server->handle_ == nullptr || (p->due && !p->due()) ||
            p->queued.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        const auto run = [](void *a) {
            auto *q = static_cast<Periodic *>(a);
            q->fn();
            q->queued.store(false, std::memory_order_release);
        };
        if (httpd_queue_work(static_cast<httpd_handle_t>(p->server->handle_), run, p) != ESP_OK) {
            p->queued.store(false, std::memory_order_release);
        }
    };
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) != ESP_OK ||
        esp_timer_start_periodic(timer, uint64_t{periodMs} * 1000) != ESP_OK) {
        return false;
    }
    periodic_.push_back(std::move(job));
    return true;
}

int HttpServer::serve(void *raw) {
    auto *req = static_cast<httpd_req_t *>(raw);
    requests_ = requests_ + 1;

    http::Request request;
    if (!methodOf(req->method, &request.method)) {
        return sendError(req, 405, "method_not_allowed", "Method not allowed");
    }

    const std::string_view uri(req->uri);
    const size_t queryAt = uri.find('?');
    std::string path;
    if (!decodePath(uri.substr(0, queryAt), &path)) {
        return sendError(req, 400, "bad_request", "Malformed path");
    }
    request.path = path;
    if (queryAt != std::string_view::npos) {
        request.query = uri.substr(queryAt + 1);
    }

    // The matched route's limit (policy table), checked before any body byte
    // is read. One request at a time on httpd, so at most one buffer exists.
    const size_t limit = std::min<size_t>(router_->bodyLimit(request.method, request.path),
                                          hubbo::auth::maxRequestBody());
    const size_t length = req->content_len;
    if (length > limit) {
        return sendError(req, 413, "payload_too_large",
                         limit == 0 ? std::string("This request takes no body")
                                    : "Body exceeds " + std::to_string(limit) + " bytes");
    }
    std::unique_ptr<char, decltype(&heap_caps_free)> body(nullptr, &heap_caps_free);
    if (length > 0) {
        body.reset(static_cast<char *>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM)));
        if (!body) {
            return sendError(req, 503, "busy", "No memory for the request body");
        }
    }
    size_t received = 0;
    int timeouts = 0;
    while (received < length) {
        const int n = httpd_req_recv(req, body.get() + received, length - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= kRecvTimeoutRetries) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;  // closes the connection
        }
        received += static_cast<size_t>(n);
    }
    request.body = std::string_view(body.get(), length);

    EspHeaders headers(req);
    request.headers = &headers;
    request.clientKey = clientKey(req);
    if (authenticator_ != nullptr) {
        request.context = authenticator_->authenticate(request);
    }

    http::Response response;
    router_->dispatch(request, response);
    return send(req, response);
}

}  // namespace hubbo::fw
