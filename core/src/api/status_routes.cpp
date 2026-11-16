// /status, /activity-runs and /remote/*.

#include <memory>

#include "hubbo/config/service.h"
#include "hubbo/engine/run_history.h"
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

// The runs, heap-allocated (about 14 KB on the board: never on the httpd stack). A
// runtime without a history has none.
std::unique_ptr<ActivityRuns> readRuns(const ApiContext &ctx) {
    auto runs = std::make_unique<ActivityRuns>();
    if (!ctx.runtime.activityRuns(runs.get())) runs->count = 0;
    return runs;
}

void putNullableMs(JsonObject out, const char *key, bool present, uint32_t ms) {
    if (present) {
        out[key] = ms;
    } else {
        out[key] = nullptr;
    }
}

// What a run and /status last_run share: id, activity_id (null for a global
// macro), kind, and a macro's button and trigger (null otherwise).
template <typename Run>
void putRunHead(JsonObject out, const Run &run) {
    out["id"] = run.id;
    putNullableId(out, "activity_id", run.activityId.view());
    out["kind"] = sequenceOriginName(run.kind);
    if (run.kind == SequenceOrigin::Macro) {
        out["button"] = remote::buttonName(run.trigger.button);
        out["trigger"] = run.trigger.kind == remote::EventKind::Hold ? "long_press" : "press";
    } else {
        out["button"] = nullptr;
        out["trigger"] = nullptr;
    }
    out["outcome"] = runOutcomeName(run.outcome);
}

void putRun(JsonObject out, const ActivityRun &run, uint32_t nowMs) {
    putRunHead(out, run);
    out["started_ago_ms"] = static_cast<uint32_t>(nowMs - run.startedMs);  // wraps safely
    putNullableMs(out, "duration_ms", run.outcome != RunOutcome::Running, run.durationMs);
    JsonArray steps = out["steps"].to<JsonArray>();
    for (size_t i = 0; i < run.stepCount; i++) {
        const RunStep &step = run.steps[i];
        JsonObject s = steps.add<JsonObject>();
        s["index"] = i;
        putNullableId(s, "device_id", step.deviceId.view());
        s["command_id"] = step.commandId;
        s["result"] = stepResultName(step.result);
        s["error"] = step.error == ErrorCode::Ok ? nullptr : errorCodeName(step.error);
        putNullableMs(s, "duration_ms", step.hasDuration(), step.durationMs);
    }
}

// /status `last_run`: the newest run in brief, or null.
void putLastRun(JsonObject root, const ApiContext &ctx) {
    LastRun run;
    if (!ctx.runtime.lastRun(&run)) {
        root["last_run"] = nullptr;
        return;
    }
    JsonObject last = root["last_run"].to<JsonObject>();
    putRunHead(last, run);
    last["failed_steps"] = run.failedSteps;
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

    sink.add("/api/v1/activity-runs", auth::Operation::ListActivityRuns, [&ctx](auto &, auto &, auto &out) {
        const auto runs = readRuns(ctx);
        JsonDocument doc;
        JsonArray list = doc["runs"].to<JsonArray>();
        for (size_t i = 0; i < runs->count; i++) putRun(list.add<JsonObject>(), runs->runs[i], runs->nowMs);
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
        root["selected_slot"] = smartSlotName(snap.selectedSlot);
        putLastRun(root, ctx);
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
