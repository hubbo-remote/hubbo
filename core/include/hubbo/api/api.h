#pragma once

#include <memory>
#include <string>
#include <vector>

#include "hubbo/api/authenticator.h"
#include "hubbo/auth/credentials.h"
#include "hubbo/auth/rate_limit.h"
#include "hubbo/auth/session.h"
#include "hubbo/domain/adapter.h"
#include "hubbo/engine/engine.h"
#include "hubbo/http/route.h"
#include "hubbo/remote/input.h"

// The /api/v1 route handlers (api/openapi.yaml), shared by the firmware and
// hub-sim. Transport-free: they see http::Request/Response only.
//
// The router admits a request first (http/route.h): Host allowlist, Origin
// for unsafe methods, the policy decision, and the operation's body limit
// and JSON content type from the policy table. Handlers then parse (JSON
// nesting <= 8, else 400 malformed_json) and check shapes strictly: unknown
// fields, wrong types, missing fields -> 422 validation_failed with JSON
// Pointer `details`.
namespace hubbo::config {
class ConfigService;
}

namespace hubbo::api {

// Live hub state for /status and /remote/status, copied by the platform
// from the engine/RF task (the HTTP task must not read engine internals).
// -1 / empty means unknown.
struct RuntimeSnapshot {
    uint64_t uptimeMs = 0;
    std::string activeActivityId;  // "" = none
    bool linkUp = true;
    std::string ip;

    struct Remote {
        bool paired = false;
        int channel = -1;
        int64_t lastPacketAgeMs = -1;
        remote::ButtonSet held = 0;
        bool hasLastEvent = false;
        remote::RemoteEvent lastEvent{};
        uint32_t lastEventAgeMs = 0;
        uint32_t badChecksums = 0;
        uint32_t unknownCodes = 0;
    } remote;

    struct Diagnostics {
        int64_t heapFree = -1;
        int64_t heapMinFree = -1;
        int64_t psramFree = -1;
        uint32_t queueOverflows = 0;
        uint32_t latencyCount = 0;  // 0 = no latency stats
        float p50Ms = 0, p95Ms = 0, p99Ms = 0;
    } diagnostics;
};

// What the API needs from the running hub besides configuration. The
// platform implements it (hub-sim: SimHub; firmware: the engine task bridge).
class Runtime {
public:
    virtual ~Runtime() = default;

    // Submit to the engine and wait until request.done(). Return false ONLY
    // if submit() fails (mailbox full; the request is then untouched and the
    // API answers 503 busy). Once submitted, never return before done():
    // the request lives on the handler's stack and the engine writes its
    // reply into it, so returning early leaves the engine writing into a
    // dead frame. The engine bounds the wait itself (transitions finish on
    // its next poll, executions within the action timeout, 5 s); a hung
    // engine trips the watchdog. Firmware: submit, then vTaskDelay-poll
    // done(). hub-sim: submit, then advance simulated time until done().
    virtual bool run(EngineRequest &request) = 0;

    virtual RuntimeSnapshot snapshot() const = 0;

    // Registered adapters (capabilities come from commandsFor()).
    virtual const AdapterSet &adapters() const = 0;

    // IntegrationState from api/openapi.yaml ("connected", "disconnected",
    // ...) and a short human detail (no secrets, no URLs with credentials).
    virtual const char *integrationState(std::string_view integrationId) const = 0;
    virtual std::string integrationDetail(std::string_view integrationId) const {
        (void)integrationId;
        return {};
    }
    // "available", "unavailable", "stale" or "unknown".
    virtual const char *deviceAvailability(const DeviceSpec &device) const {
        (void)device;
        return "unknown";
    }
};

// Identity of this hub for /status and the auth responses.
struct HubInfo {
    std::string version = "0.0.0";
    std::string build;
    std::string board;
    std::string hostname;
    bool simulatorNoAuth = false;  // reported as auth_mode simulator_no_auth
};

struct ApiContext {
    auth::Credentials &credentials;
    auth::SessionStore &sessions;
    auth::LoginRateLimiter &loginLimiter;
    const Clock &clock;
    config::ConfigService &config;
    Runtime &runtime;
    HubInfo info;
    // What POST /admin/reset restores (the board's first-boot config). Null:
    // an empty config (ConfigService adds the built-in integrations).
    const ConfigSpec *factoryDefaults = nullptr;
};

// Owns the route handlers; keep it alive as long as the router.
class Api {
public:
    explicit Api(ApiContext context);
    ~Api();
    Api(const Api &) = delete;
    Api &operator=(const Api &) = delete;

    void registerRoutes(http::Router &router);

private:
    ApiContext ctx_;
    std::vector<std::unique_ptr<http::RouteHandler>> handlers_;
};

inline void registerApiRoutes(http::Router &router, Api &api) { api.registerRoutes(router); }

}  // namespace hubbo::api
