#pragma once

// esp_http_server adapter for the transport-independent hubbo::http::Router.
//
// Every /api/* request is turned into an http::Request (decoded path, query,
// body, headers), authenticated by the injected RequestAuthenticator, and
// dispatched to the Router; the Response is written back. No routing or
// auth logic lives here.
//
// Task: esp_http_server runs every handler on its own single task, pinned
// to core 0 below the network stack, so a slow handler (config compile,
// flash write) never runs on the RF/engine task (core 1). Handlers reach the
// engine only through its mailbox / SnapshotStore. post() and every() run
// other work on that same task, serialized with the handlers: the one way to
// touch the router, ConfigService or anything else the handlers own.
//
// Limits: the body may be as large as the matched route allows
// (Router::bodyLimit, from the auth policy table; a firmware update's is the
// OTA slot size). Anything larger gets 413 before a byte is read. The body
// is read into a PSRAM buffer allocated per request; no memory for it: 503.
// URI and header sizes are bounded by the framework (CONFIG_HTTPD_MAX_URI_LEN
// 512, CONFIG_HTTPD_MAX_REQ_HDR_LEN 1024 in arduino-esp32 3.3.12).
//
// Plain HTTP on the LAN (`secure` stays false); HTTPS is out of scope.

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "hubbo/http/route.h"

namespace hubbo::fw {

class HttpServer {
public:
    static constexpr uint16_t kPort = 80;
    static constexpr int kCore = 0;
    static constexpr unsigned kPriority = 5;  // tskIDLE_PRIORITY + 5; lwIP runs at 18
    // Internal RAM (httpd writes flash). 8 KB overflowed adding an HA
    // media_player: the config commit's parse runs deep in the handler.
    static constexpr size_t kStackBytes = 16384;

    // `router` and `authenticator` must outlive the server. A null
    // authenticator treats every caller as anonymous (fail closed).
    bool begin(const http::Router &router, http::RequestAuthenticator *authenticator);
    bool running() const { return handle_ != nullptr; }

    // Runs `fn` once on the httpd task. False if the server isn't running or
    // its work queue is full.
    bool post(std::function<void()> fn);

    // Runs `fn` on the httpd task every `periodMs` (an esp_timer queues it;
    // at most one run is queued at a time). `due`, if given, is checked on
    // the esp_timer task first and must be cheap and thread-safe; returning
    // false skips that period without waking httpd.
    bool every(uint32_t periodMs, std::function<void()> fn, std::function<bool()> due = nullptr);

#ifdef HUBBO_DEV_CONSOLE
    // A plain-text GET outside the API router and its auth. Runs on httpd.
    // For read-only diagnostics only. `query` is raw (not decoded).
    using DebugGet = std::function<std::string(std::string_view query)>;
    bool addDebugGet(const char *uri, DebugGet handler);
#endif

    // The embedded web admin (web_assets.h) on GET /*. Register it last:
    // httpd matches in registration order, so /api/* and /debug/* win.
    // Hashed /a/ files are immutable; everything else is revalidated with
    // the build's ETag (304). A path that isn't a file gets index.html when
    // the client wants HTML (the SPA's routes), else 404 (always under /a/).
    // Bodies are sent gzipped, straight from flash.
    bool serveWebAdmin();

    uint32_t requestCount() const { return requests_; }

private:
    friend struct HttpServerAccess;
    int serve(void *req);  // esp_err_t (httpd_req_t *)
    int serveStatic(void *req);

    struct Periodic {
        HttpServer *server;
        std::function<void()> fn;
        std::function<bool()> due;
        std::atomic<bool> queued{false};
    };
    std::vector<std::unique_ptr<Periodic>> periodic_;  // live for the program
#ifdef HUBBO_DEV_CONSOLE
    std::vector<std::unique_ptr<DebugGet>> debugGets_;  // live for the program
#endif

    void *handle_ = nullptr;  // httpd_handle_t
    const http::Router *router_ = nullptr;
    http::RequestAuthenticator *authenticator_ = nullptr;
    volatile uint32_t requests_ = 0;
};

}  // namespace hubbo::fw
