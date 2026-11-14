#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "hubbo/api/authenticator.h"
#include "hubbo/api/factory_reset_port.h"
#include "hubbo/api/firmware_update_port.h"
#include "hubbo/api/remote_port.h"
#include "hubbo/api/restart_port.h"
#include "hubbo/auth/credentials.h"
#include "hubbo/auth/session.h"
#include "hubbo/config/hub.h"
#include "hubbo/domain/adapter.h"
#include "hubbo/engine/engine.h"
#include "hubbo/http/route.h"
#include "hubbo/integrations/ble_hosts.h"
#include "hubbo/remote/input.h"

// The /api/v1 route handlers (api/openapi.yaml), shared by the firmware and
// hub-sim. Transport-free: they see http::Request/Response only.
//
// The router admits a request first (http/route.h): Origin for unsafe
// methods, the policy decision, and the operation's body limit
// and content type from the policy table. Handlers then parse (JSON
// nesting <= 8, else 400 malformed_json) and check shapes strictly: unknown
// fields, wrong types, missing fields -> 422 validation_failed with JSON
// Pointer `details`.
namespace hubbo::config {
class ConfigService;
}
namespace hubbo::integrations {
class HomeAssistantClient;
class MqttBridge;
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
    bool simulatorNoAuth = false;  // reported as auth_mode simulator_no_auth
};

struct ApiContext {
    auth::Credentials &credentials;
    auth::SessionStore &sessions;
    const Clock &clock;
    config::ConfigService &config;
    Runtime &runtime;
    HubInfo info;

    // Phase 3 ports; null where a platform has none (the routes answer 503).
    // The remote radio (GET/PUT /admin/remote..., the pair scan).
    RemotePort *remote = nullptr;
    // The Home Assistant client for integration "ha_main": settings
    // (configure()), discovery. Any-task calls only.
    integrations::HomeAssistantClient *homeAssistant = nullptr;
    // Where the HA access token lives (key kHaTokenKey). Usually the same
    // store as the credentials'. The platform reads it at boot and passes it
    // to homeAssistant->configure() with the integration's URL.
    auth::SecretStore *secrets = nullptr;

    // Internal (Api sets it): the latest HA discovery, shared by the
    // discover and register routes.
    std::shared_ptr<struct DiscoveryCache> discovery;

    // The Bluetooth hosts, one per ble_hid device (the /admin/devices/{id}/pair
    // and /pairing routes, each device's `bluetooth` status); null answers
    // 503. Positional initializers that stop at `discovery` still compile.
    integrations::BleHostPort *bleHosts = nullptr;
    // Firmware updates (GET /admin/firmware, POST /admin/firmware/update);
    // null answers 503.
    FirmwareUpdatePort *firmware = nullptr;
    // The real factory reset (POST /admin/factory-reset); null answers 503.
    FactoryResetPort *factoryReset = nullptr;
    // The hub's name (GET/PUT /admin/hub, /status hubbo.name); null: the
    // routes answer 503 and /status reports the default name.
    config::HubSettings *hub = nullptr;
    // POST /admin/restart; null answers 503.
    RestartPort *restart = nullptr;
    // The MQTT bridge of integration "mqtt_main": settings changes reach it
    // through applyMqttSettings(). Null: they're saved, nothing applies them.
    integrations::MqttBridge *mqtt = nullptr;
};

// SecretStore key of the "ha_main" access token. Never in config, backups,
// responses or logs.
constexpr const char *kHaTokenKey = "ha_token";
constexpr const char *kHaIntegrationId = "ha_main";

// The WebSocket URL for an HA base URL: "http://ha.local:8123" ->
// "ws://ha.local:8123/api/websocket" (https -> wss). "" if it isn't one.
std::string homeAssistantSocketUrl(std::string_view baseUrl);
// Configures `client` from the "ha_main" integration (no URL while it's
// missing or disabled) and the stored token (none unless the read is Ok).
// The platform calls it at boot; PUT /admin/integrations/ha_main calls it
// after a change. Returns the token read's result, for a boot log line.
auth::SecretRead applyHomeAssistantSettings(const config::ConfigService &config, const auth::SecretStore *secrets,
                                            integrations::HomeAssistantClient &client);

// SecretStore key of the "mqtt_main" broker password. Never in config,
// backups, responses or logs.
constexpr const char *kMqttPasswordKey = "mqtt_password";
// Configures `bridge` from the "mqtt_main" integration (enabled, host, port,
// username) and the stored password (none unless the read is Ok). The
// platform calls it at boot; the API after a change (PUT, restore, reset).
// Returns the password read's result, for a boot log line.
auth::SecretRead applyMqttSettings(const config::ConfigService &config, const auth::SecretStore *secrets,
                                   integrations::MqttBridge &bridge);

// Owns the route handlers; keep it alive as long as the router.
class Api {
public:
    explicit Api(ApiContext context);
    ~Api();
    Api(const Api &) = delete;
    Api &operator=(const Api &) = delete;

    void registerRoutes(http::Router &router);
    // Called between steps of the work a discovery GET does on the HTTP task
    // (normalizing up to 2048 candidates), so the platform can let other
    // tasks and the watchdog run (the board: vTaskDelay(1)). Optional.
    void setYield(std::function<void()> yield);

private:
    ApiContext ctx_;
    std::vector<std::unique_ptr<http::RouteHandler>> handlers_;
};

inline void registerApiRoutes(http::Router &router, Api &api) { api.registerRoutes(router); }

}  // namespace hubbo::api
