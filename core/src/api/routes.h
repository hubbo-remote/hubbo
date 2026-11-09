#pragma once

#include <functional>

// Route groups of the API (not a public header). Each add*Routes registers
// its operations' handlers on the sink.

#include <memory>
#include <vector>

#include "hubbo/api/api.h"
#include "hubbo/config/service.h"
#include "hubbo/ha/services.h"
#include "hubbo/ha/snapshot.h"

namespace hubbo::api {

struct RouteSink {
    http::Router &router;
    std::vector<std::unique_ptr<http::RouteHandler>> &handlers;  // owned by Api
    config::ConfigService &config;
    const Clock &clock;

    // `fn(request, params, response)`; stored as-is (no std::function).
    // Every handler first lets the config settle (ConfigService::refresh():
    // a boot load that failed on storage is retried once the medium is
    // back), so state and status are current on every request.
    template <typename F>
    void add(const char *pattern, auth::Operation op, F fn) {
        struct Handler : http::RouteHandler {
            Handler(config::ConfigService &c, const Clock &k, F f) : config(c), clock(k), fn(std::move(f)) {}
            void handle(const http::Request &r, const http::PathParams &p, http::Response &out) override {
                config.refresh(clock.monotonicMs());  // backs off internally; cheap per request
                fn(r, p, out);
            }
            config::ConfigService &config;
            const Clock &clock;
            F fn;
        };
        handlers.push_back(std::make_unique<Handler>(config, clock, std::move(fn)));
        router.add(pattern, op, *handlers.back());
    }
};

// The latest Home Assistant discovery, kept in memory (HTTP task only)
// until the next discovery or a reboot. The register route takes a new
// device's capabilities snapshot from here: the HTTP task never calls HA.
struct DiscoveryCache {
    enum class State : uint8_t { Idle, Running, Done, Failed };
    struct Candidate {
        std::string entityId, uniqueId, name, domain, platform;
        ha::CapabilitySnapshot snapshot;      // what registration stores
        std::vector<std::string> commandIds;  // what the snapshot offers
        std::vector<std::string> skipped;     // services matching but not offered
        size_t droppedOptions = 0;
    };
    State state = State::Idle;
    std::string error;  // Failed
    std::vector<Candidate> candidates;
    size_t truncated = 0;    // allowlisted entities left out over the client's cap (Done)
    uint32_t elapsedMs = 0;  // how long the discovery took (Done)
    uint32_t generation = 0; // the client's discovery generation it came from (Done)
    std::function<void()> yield;  // Api::setYield (kept across resets)

    // `state` with nothing from an earlier discovery (yield stays).
    void reset(State next) {
        state = next;
        error.clear();
        candidates.clear();
        truncated = 0;
        elapsedMs = 0;
        generation = 0;
    }
    // A Done candidate, only while `currentGeneration` (the client's
    // discoveryGeneration()) is still the one it came from: other settings,
    // a reconnect or a newer discovery make the cache stale.
    const Candidate *find(std::string_view entityId, uint32_t currentGeneration) const;
};

void addIntegrationRoutes(RouteSink &sink, ApiContext &ctx);
// Before replacing the whole config (restore, factory reset) with `next`:
// erases the HA token if "ha_main"'s URL would change, so it's never sent to
// a host it wasn't entered for. False after sending 503 (nothing changed).
bool forgetTokenForNewHaUrl(ApiContext &ctx, const ConfigSpec &next, http::Response &out);
// After a config change: the HA client follows the stored settings. With
// `haChanged` (another URL, token or enabled state; a restore or reset) the
// discovery of the old settings is cancelled and its candidates dropped, so
// nothing is registered from a different Home Assistant.
void reapplyHomeAssistant(ApiContext &ctx, bool haChanged);
void addRemoteRoutes(RouteSink &sink, ApiContext &ctx);       // remote endpoint and pair scan
void addAuthRoutes(RouteSink &sink, ApiContext &ctx);      // setup, login, logout, session, password, tokens
void addRegistryRoutes(RouteSink &sink, ApiContext &ctx);  // devices, capabilities, execute, integrations
void addActivityRoutes(RouteSink &sink, ApiContext &ctx);  // activities, global bindings, activate, off
void addStatusRoutes(RouteSink &sink, ApiContext &ctx);    // status, remote status/buttons
void addBackupRoutes(RouteSink &sink, ApiContext &ctx);    // backup download, restore
void addUnimplementedRoutes(RouteSink &sink);              // clear 501s, no fake success

}  // namespace hubbo::api
