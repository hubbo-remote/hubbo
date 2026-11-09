#pragma once

// Route groups of the API (not a public header). Each add*Routes registers
// its operations' handlers on the sink.

#include <memory>
#include <vector>

#include "hubbo/api/api.h"
#include "hubbo/config/service.h"

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

void addAuthRoutes(RouteSink &sink, ApiContext &ctx);      // setup, login, logout, session, password, tokens
void addRegistryRoutes(RouteSink &sink, ApiContext &ctx);  // devices, capabilities, execute, integrations
void addActivityRoutes(RouteSink &sink, ApiContext &ctx);  // activities, global bindings, activate, off
void addStatusRoutes(RouteSink &sink, ApiContext &ctx);    // status, remote status/buttons
void addBackupRoutes(RouteSink &sink, ApiContext &ctx);    // backup download, restore
void addUnimplementedRoutes(RouteSink &sink);              // clear 501s, no fake success

}  // namespace hubbo::api
