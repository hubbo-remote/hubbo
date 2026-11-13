// /status and /remote/*.

#include "hubbo/config/service.h"
#include "hubbo/remote/button.h"
#include "hubbo/remote/input.h"
#include "routes.h"
#include "support.h"

namespace hubbo::api {

namespace {

void putRemote(JsonObject r, const RuntimeSnapshot::Remote &remote) {
    r["paired"] = remote.paired;
    if (remote.channel >= 0) r["channel"] = remote.channel;
    if (remote.lastPacketAgeMs >= 0) {
        r["last_packet_age_ms"] = remote.lastPacketAgeMs;
    } else {
        r["last_packet_age_ms"] = nullptr;
    }
    JsonArray held = r["held"].to<JsonArray>();
    for (size_t i = 0; i < remote::kButtonCount; i++) {
        const auto button = static_cast<remote::Button>(i);
        if (remote.held & remote::buttonBit(button)) held.add(remote::buttonName(button));
    }
    if (remote.hasLastEvent) {
        JsonObject e = r["last_event"].to<JsonObject>();
        e["button"] = remote::buttonName(remote.lastEvent.button);
        e["kind"] = remote::eventKindName(remote.lastEvent.kind);
        e["age_ms"] = remote.lastEventAgeMs;
        e["timed_out"] = remote.lastEvent.timedOut;
    } else {
        r["last_event"] = nullptr;
    }
    JsonObject counters = r["counters"].to<JsonObject>();
    counters["bad_checksum"] = remote.badChecksums;
    counters["unknown_code"] = remote.unknownCodes;
}

}  // namespace

void addStatusRoutes(RouteSink &sink, ApiContext &ctx) {
    sink.add("/api/v1/remote/buttons", auth::Operation::ListRemoteButtons,
             [](const http::Request &, const http::PathParams &, http::Response &out) {
                 JsonDocument doc;
                 JsonArray buttons = doc["buttons"].to<JsonArray>();
                 for (size_t i = 0; i < remote::kButtonCount; i++) {
                     const remote::ButtonInfo &info = remote::buttonInfo(static_cast<remote::Button>(i));
                     JsonObject b = buttons.add<JsonObject>();
                     b["name"] = info.name;
                     b["group"] = info.group == remote::kGroupKeypad ? "keypad" : "media_home";
                 }
                 JsonArray kinds = doc["event_kinds"].to<JsonArray>();
                 for (size_t i = 0; i < remote::kEventKindCount; i++) {
                     kinds.add(remote::eventKindName(static_cast<remote::EventKind>(i)));
                 }
                 doc["hold_ms"] = remote::InputConfig{}.holdMs;
                 sendJson(out, 200, doc);
             });


    sink.add("/api/v1/remote/status", auth::Operation::GetRemoteStatus, [&ctx](auto &, auto &, auto &out) {
        JsonDocument doc;
        putRemote(doc.to<JsonObject>(), ctx.runtime.snapshot().remote);
        sendJson(out, 200, doc);
    });

    sink.add("/api/v1/status", auth::Operation::GetStatus, [&ctx](auto &, auto &, auto &out) {
        const RuntimeSnapshot snap = ctx.runtime.snapshot();
        JsonDocument doc;
        JsonObject root = doc.to<JsonObject>();
        const config::HubIdentity identity = ctx.hub != nullptr ? ctx.hub->identity() : config::HubIdentity();
        JsonObject hubbo = doc["hubbo"].to<JsonObject>();
        hubbo["name"] = identity.name;  // the default without HubSettings
        hubbo["version"] = ctx.info.version;
        if (!ctx.info.build.empty()) hubbo["build"] = ctx.info.build;
        hubbo["board"] = ctx.info.board;
        doc["uptime_s"] = snap.uptimeMs / 1000;
        putTimestamp(root, "time", ctx.clock.unixSeconds());
        doc["auth_mode"] = authMode(ctx);
        putConfigState(doc["config"].to<JsonObject>(), ctx);
        doc["auth_state"] = auth::credentialStateName(ctx.credentials.state());
        putNullableId(root, "active_activity_id", snap.activeActivityId);
        JsonObject network = doc["network"].to<JsonObject>();
        network["link"] = snap.linkUp ? "up" : "down";
        if (!snap.ip.empty()) network["ip"] = snap.ip;
        if (ctx.hub != nullptr) network["hostname"] = identity.shownHostname();  // what mDNS advertises (live)
        putRemote(doc["remote"].to<JsonObject>(), snap.remote);

        JsonArray integrations = doc["integrations"].to<JsonArray>();
        putIntegrationSummary(integrations, ctx, nullptr);
        for (const auto &i : ctx.config.config().integrations) {
            if (i.id != kSystemIntegrationId) putIntegrationSummary(integrations, ctx, &i);
        }

        const auto &d = snap.diagnostics;
        JsonObject diag = doc["diagnostics"].to<JsonObject>();
        if (d.heapFree >= 0) diag["heap_free"] = d.heapFree;
        if (d.heapMinFree >= 0) diag["heap_min_free"] = d.heapMinFree;
        if (d.psramFree >= 0) diag["psram_free"] = d.psramFree;
        diag["queue_overflows"] = d.queueOverflows;
        if (d.latencyCount > 0) {
            JsonObject l = diag["dispatch_latency"].to<JsonObject>();
            l["count"] = d.latencyCount;
            l["p50_ms"] = d.p50Ms;
            l["p95_ms"] = d.p95Ms;
            l["p99_ms"] = d.p99Ms;
        } else {
            diag["dispatch_latency"] = nullptr;
        }
        sendJson(out, 200, doc);
    });
}

}  // namespace hubbo::api
