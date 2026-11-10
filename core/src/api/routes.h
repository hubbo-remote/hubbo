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

// The state of an async HA fetch (a discovery, an entity's details).
enum class FetchState : uint8_t { Idle, Running, Done, Failed };
const char *fetchStateName(FetchState state);  // "idle", "running", "done", "failed"

// What a discovery candidate and an entity's details both show.
struct HaEntity {
    std::string entityId, uniqueId, name, domain;  // uniqueId: details only, "" if YAML-only
    std::string platform;                          // "" if YAML-only
    std::vector<std::string> commandIds;           // what it offers
    size_t droppedOptions = 0;
};

// One HA entity's details (POST/GET .../entities/{entity_id}/details): what
// registering it stores. Fetched per entity, since discovery has no services.
struct EntityDetails : HaEntity {  // entityId: the requested entity (all but Idle)
    FetchState state = FetchState::Idle;
    std::string error;                 // Failed
    ha::CapabilitySnapshot snapshot;   // Done: what registration stores
    std::vector<std::string> skipped;  // Done: services matching but not offered

    void reset(FetchState next, std::string entity = {}) {
        *this = EntityDetails{};
        state = next;
        entityId = std::move(entity);
    }
};

// The latest Home Assistant discovery and entity details, kept in memory
// (HTTP task only) until the next request, an HA settings change or a
// reboot. The register route takes a device's snapshot from `details`: the
// HTTP task never calls HA.
struct DiscoveryCache {
    using State = FetchState;
    struct Candidate : HaEntity {  // commandIds: a preview, domain commands only
        std::string state;         // HA's current state ("on")
    };
    State state = State::Idle;
    std::string error;  // Failed
    std::vector<Candidate> candidates;
    size_t truncated = 0;    // allowlisted entities left out over the client's cap (Done)
    uint32_t elapsedMs = 0;  // how long the discovery took (Done)
    uint32_t generation = 0; // the client's discovery generation it came from (Done)
    std::function<void()> yield;  // Api::setYield (kept across resets)
    EntityDetails details;        // not touched by reset()

    // `state` with nothing from an earlier discovery (yield and details stay).
    void reset(State next) {
        state = next;
        error.clear();
        candidates.clear();
        truncated = 0;
        elapsedMs = 0;
        generation = 0;
    }
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
// The finished details of `entityId` from the current HA settings (what
// registering or re-linking it stores), or null: fetch them first.
const EntityDetails *completedDetails(ApiContext &ctx, std::string_view entityId);
// The HA device registered for this entity: by entity_id, else (an entity
// renamed in HA) by unique_id within its platform, since HA keeps unique_id
// unique only per platform. Discovery candidates have no unique_id, so they
// match by entity_id only; the details find a renamed one.
const DeviceSpec *registeredDevice(const ApiContext &ctx, std::string_view entityId, std::string_view uniqueId = {},
                                   std::string_view platform = {});
void addRemoteRoutes(RouteSink &sink, ApiContext &ctx);       // remote endpoint and pair scan
void addBleRoutes(RouteSink &sink, ApiContext &ctx);          // BLE HID pairing
void addAuthRoutes(RouteSink &sink, ApiContext &ctx);      // setup, login, logout, session, password, tokens
void addRegistryRoutes(RouteSink &sink, ApiContext &ctx);  // devices, capabilities, execute, integrations
void addActivityRoutes(RouteSink &sink, ApiContext &ctx);  // activities, global bindings, activate, off
void addStatusRoutes(RouteSink &sink, ApiContext &ctx);    // status, remote status/buttons
void addBackupRoutes(RouteSink &sink, ApiContext &ctx);    // backup download, restore
void addFirmwareRoutes(RouteSink &sink, ApiContext &ctx);  // firmware status and update, factory reset

}  // namespace hubbo::api
