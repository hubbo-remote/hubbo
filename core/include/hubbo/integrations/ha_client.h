#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "hubbo/domain/error.h"
#include "hubbo/ha/capabilities.h"
#include "hubbo/integrations/json_scan.h"
#include "hubbo/integrations/websocket_transport.h"

// Home Assistant WebSocket API client (docs/design/phase3-home-assistant.md,
// section 1; phase4-web-admin.md 3b): the auth handshake, numbered commands
// (call_service, discovery, entity details), liveness pings and reconnects
// with backoff.
//
// Threading: poll(), callService() and discover() run on the integration
// task, as do the transport callbacks and every `done` callback. state(),
// configure(), requestDiscovery() and takeDiscovery() are safe from any
// task (httpd, config). Nothing here runs on the RF/engine task; the HA
// adapter (ha_adapter.h) bridges to it without blocking.
//
// Liveness: HA doesn't ping clients, so the client sends {"type":"ping"}
// every kPingIntervalMs while Connected; no pong within kPongTimeoutMs
// closes the connection (Unreachable, then the backoff).
//
// Discovery reads what the add-device picker shows, and as little else as
// it can (on the board the time is per byte): the compact entity registry
// (config/entity_registry/list_for_display: enabled entities, platform,
// name), then every current state (subscribe_entities, then unsubscribe;
// YAML-only entities have a state but no registry entry). Actions
// (get_services) aren't part of it: entity details fetch them for the one
// entity being registered or refreshed.
//
// Neither big message is parsed in one go (it would hold the task for
// seconds): it is walked element by element, at most kParseStepEntities per
// poll(), so the integration task yields between steps (hasWork() tells its
// loop not to sleep meanwhile). Only allowlisted domains are kept.
//
// The access token is used only in the auth message. It never appears in
// logs, errors or anything this class returns.
namespace hubbo::integrations {

enum class HaState : uint8_t {
    NotConfigured,   // no URL or no token
    Connecting,      // transport connecting
    Authenticating,  // connected, auth handshake in progress
    Connected,       // auth_ok: commands work
    AuthFailed,      // auth_invalid: no retry until configure() changes something
    Unreachable,     // connect failed or dropped: retry with backoff
};
// "not_configured", "connecting", "authenticating", "connected", "auth_failed", "unreachable"
const char *haStateName(HaState state);

// One discovered entity (an add-device candidate).
struct HaCandidate {
    std::string entityId;
    std::string name;      // friendly_name, else the registry name, else ""
    std::string domain;    // "media_player"
    std::string platform;  // the integration behind it ("sonos"); "" for a YAML-only entity
    std::string state;     // "on", "playing", "unavailable"...
    std::string deviceClass;  // the state's device_class ("tv", "blind"), "" if none
    ha::EntityInfo info;   // supported_features and the attributes that shape params
};

struct HaDiscovery {
    std::vector<HaCandidate> candidates;  // allowlisted domains only, sorted by entity_id
    // Allowlisted entities left out because kMaxCandidates were already
    // found (0: the list is complete). Shown, never silent.
    size_t truncated = 0;
    uint32_t elapsedMs = 0;  // request to result, parse steps and yields included (for logs)
    // The mailbox request this answers (discoveryGeneration() when it was
    // requested); 0 from discover().
    uint32_t generation = 0;
};

// One entity as registration and refresh need it, fetched fresh.
struct HaEntityDetails {
    std::string entityId;
    std::string uniqueId;  // registry unique_id; "" for a YAML-only entity
    std::string platform;  // registry platform; "" for YAML-only (no integration services)
    std::string name;      // as HaCandidate::name
    ha::EntityInfo info;   // from its current state
    // get_services' result as HA sent it (JSON object, domain -> services),
    // for integration-specific services (ha/). Not interpreted here. A few
    // hundred KB (large allocations go to PSRAM on the board); free it once
    // normalized.
    std::string services;
    // frontend/get_translations' `resources` for `platform` (category
    // services, English), as HA sent it: a flat JSON object,
    // "component.<platform>.services.<service>.name" (and .description,
    // .fields.<field>.name, ...) -> string. "" when there's no platform or
    // the fetch failed: the services' own names are the fallback. Free it
    // once normalized, like `services`.
    std::string translations;
};

enum class DiscoveryStatus : uint8_t { Idle, Running, Done, Failed };
const char *discoveryStatusName(DiscoveryStatus status);  // "idle", "running", "done", "failed"

class HomeAssistantClient : private WebSocketTransport::Listener {
public:
    static constexpr uint32_t kCommandTimeoutMs = 5000;
    static constexpr uint32_t kDiscoveryTimeoutMs = 30000;  // per discovery command
    static constexpr uint32_t kAuthTimeoutMs = 10000;
    static constexpr uint32_t kPingIntervalMs = 30000;
    static constexpr uint32_t kPongTimeoutMs = 10000;
    static constexpr uint32_t kFirstRetryMs = 1000;
    static constexpr uint32_t kMaxRetryMs = 60000;
    static constexpr size_t kMaxPendingCommands = 16;
    // A big home fits (a candidate is a few hundred bytes; the board's large
    // allocations go to PSRAM).
    static constexpr size_t kMaxCandidates = 2048;
    static constexpr size_t kParseStepEntities = 25;

    // Milliseconds, the same time base as poll()'s nowMs (firmware:
    // esp_timer_get_time() / 1000). Read fresh at every poll() and every
    // transport callback, so a deadline set after a slow transport.poll()
    // (a TLS connect can take seconds) counts from when it really started.
    // Without one, poll()'s nowMs is the only clock.
    using Clock = std::function<uint64_t()>;
    explicit HomeAssistantClient(WebSocketTransport &transport, Clock clock = {});
    ~HomeAssistantClient() override;

    // Any task. New settings take effect at the next poll(): an open
    // connection is closed and the client reconnects at once (any change,
    // verifyTls included, clears AuthFailed). "" url or token: NotConfigured.
    // verifyTls goes to transport.connect() (ConnectOptions).
    void configure(std::string url, std::string token, bool verifyTls = true);
    // Any task: reconnect now with the current settings, as a settings
    // change does (drops the connection, skips the backoff, clears AuthFailed
    // and the stale entities).
    void reconnect();
    HaState state() const { return state_.load(std::memory_order_acquire); }

    // Integration task, every loop: settings, reconnect backoff, pings,
    // command timeouts, one discovery parse step, a requested discovery.
    void poll(uint64_t nowMs);
    // Integration task: there is work the next poll() should do at once (a
    // discovery is being parsed); the loop shouldn't sleep meanwhile.
    bool hasWork() const;
    // Called between discovery parse steps, on the integration task (e.g.
    // vTaskDelay(1) on the board). Optional.
    void setYield(std::function<void()> yield) { yield_ = std::move(yield); }

    // Integration task. Sends call_service; `done` gets Ok (HA's result was
    // success; HA also says so for an entity it doesn't have, see
    // entityStale()), AdapterFailed (HA refused it), AdapterTimeout (no result in
    // kCommandTimeoutMs) or AdapterUnavailable (the connection dropped).
    // Returns Ok (sent; `done` will be called exactly once), or at once with
    // AdapterUnavailable (not Connected, or the send failed) / QueueFull
    // (kMaxPendingCommands in flight), and then `done` isn't called.
    using CallDone = std::function<void(ErrorCode result)>;
    ErrorCode callService(const ha::ServiceCall &call, CallDone done);

    // Any task. Runtime status only (never saved): the entity is missing
    // from the latest completed discovery. Discovery is the only source: HA
    // answers call_service on an unknown entity with success. False before
    // any discovery, and after new settings.
    bool entityStale(std::string_view entityId) const;

    // Integration task: fetch the registry and states and build the
    // candidates. Ok (started; `done` is called exactly once: Ok, or
    // AdapterUnavailable / AdapterTimeout / AdapterFailed / MalformedJson),
    // Busy (one is running) or AdapterUnavailable (not Connected).
    using DiscoverDone = std::function<void(ErrorCode result, HaDiscovery discovery)>;
    ErrorCode discover(DiscoverDone done);

    // Integration task: one entity's registry entry (none: YAML-only), its
    // state and get_services, then, when it has a platform, that
    // integration's service translations (optional: any failure there still
    // ends Ok, without them). Ok (started; `done` is called exactly once:
    // Ok, or AdapterUnavailable / AdapterTimeout / AdapterFailed /
    // EntityNotFound (HA has no state for it) / MalformedJson), Busy (one is
    // running; a discovery may run alongside), AdapterUnavailable (not
    // Connected) or InvalidId (not a valid, allowlisted entity id). A
    // completed fetch also proves the entity exists: it isn't stale until
    // the next discovery says otherwise.
    using DetailsDone = std::function<void(ErrorCode result, HaEntityDetails details)>;
    ErrorCode entityDetails(const std::string &entityId, DetailsDone done);

    // Any task (the admin API, like the pair scan: POST starts, GET polls).
    // requestDiscovery(): Ok (the integration task starts it at its next
    // poll), Busy (one is requested or running) or AdapterUnavailable (not
    // Connected). takeDiscovery(): the status; Done hands over the result
    // (`out`) and Failed the reason (`error`), and both go back to Idle.
    ErrorCode requestDiscovery();
    DiscoveryStatus takeDiscovery(HaDiscovery *out, ErrorCode *error);
    // Any task: drop a requested or running discovery (its result is thrown
    // away; the status goes back to Idle). No-op if none.
    void cancelDiscovery();
    // Any task, the same model for one entity's details (registration and
    // refresh): request (Ok, Busy, AdapterUnavailable, InvalidId), take
    // (Idle, Running, Done hands over `out`, Failed gives `error`), cancel.
    // A cancel frees the slot at once: a request right after it is Ok. New
    // settings drop a requested, running or finished fetch.
    ErrorCode requestEntityDetails(std::string_view entityId);
    DiscoveryStatus takeEntityDetails(HaEntityDetails *out, ErrorCode *error);
    void cancelEntityDetails();

    // Any task. Bumped by requestDiscovery(), cancelDiscovery() (of a
    // running one), configure() with changed settings and reconnect(); a
    // result from an older generation is never published (a finished one
    // still in the mailbox is thrown away). A cache of candidates is current
    // while its HaDiscovery::generation equals this.
    uint32_t discoveryGeneration() const;

private:
    enum class Kind : uint8_t {
        CallService,
        Registry,         // discovery: list_for_display
        States,           // discovery: subscribe_entities (all), a subscription
        DetailsRegistry,  // details: config/entity_registry/get
        DetailsState,     // details: subscribe_entities (one), a subscription
        DetailsServices,  // details: get_services
        DetailsTranslations,  // details: frontend/get_translations (optional: a failure degrades)
    };
    static bool isSubscription(Kind k) { return k == Kind::States || k == Kind::DetailsState; }
    static bool isDiscovery(Kind k) { return k == Kind::Registry || k == Kind::States; }
    struct Pending {
        bool used = false;
        uint32_t id = 0;
        Kind kind = Kind::CallService;
        uint64_t deadlineMs = 0;
        CallDone done;  // CallService
    };
    struct Settings {
        std::string url;
        std::string token;
        bool verifyTls = true;
    };
    struct RegistryEntry {
        std::string platform;
        std::string name;
    };
    enum class Phase : uint8_t { None, Registry, States };

    // A cross-task request slot (discovery, details), under mailboxMutex_.
    // A result is published only if no cancel, newer request or new
    // settings bumped the generation since its request.
    template <typename T>
    struct Mailbox {
        bool requested = false;
        bool cancelRequested = false;
        uint32_t generation = 0;
        DiscoveryStatus status = DiscoveryStatus::Idle;
        T result;
        ErrorCode error = ErrorCode::Ok;

        ErrorCode request() {
            if (status == DiscoveryStatus::Running) return ErrorCode::Busy;  // also while requested
            requested = true;
            generation++;
            status = DiscoveryStatus::Running;
            result = T();
            error = ErrorCode::Ok;
            return ErrorCode::Ok;
        }
        void cancel() {
            if (status != DiscoveryStatus::Running) return;
            generation++;  // its result, if it still comes, is dropped
            if (requested) {
                requested = false;  // never started
            } else {
                cancelRequested = true;  // the integration task stops it
            }
            status = DiscoveryStatus::Idle;
            result = T();
        }
        void forget() {  // new settings
            generation++;
            requested = false;
            status = DiscoveryStatus::Idle;
            result = T();
        }
        void publish(uint32_t requestGeneration, ErrorCode e, T value) {
            if (requestGeneration != generation) return;
            status = e == ErrorCode::Ok ? DiscoveryStatus::Done : DiscoveryStatus::Failed;
            error = e;
            result = std::move(value);
        }
        DiscoveryStatus take(T *out, ErrorCode *e) {
            const DiscoveryStatus s = status;
            if (s == DiscoveryStatus::Done && out != nullptr) *out = std::move(result);
            if (s == DiscoveryStatus::Failed && e != nullptr) *e = error;
            if (s == DiscoveryStatus::Done || s == DiscoveryStatus::Failed) {
                status = DiscoveryStatus::Idle;
                result = T();
            }
            return s;
        }
    };

    // WebSocketTransport::Listener
    void onConnected() override;
    void onText(std::string text) override;
    void onClosed() override;

    void setState(HaState state) { state_.store(state, std::memory_order_release); }
    void readClock() {
        if (clock_) nowMs_ = clock_();
    }
    void applySettings();
    void forgetRequests();  // settings changed: drop discovery and details requests and results
    void scheduleRetry();
    void failInFlight();  // the connection is gone: fail calls, discovery and details
    void serviceMailboxes();
    // `body`: the command's JSON object without "id" (e.g. {"type":"get_states"}).
    ErrorCode sendCommand(Kind kind, const std::string &body, uint32_t timeoutMs, CallDone done);
    void unsubscribe(uint32_t subscriptionId);
    void release(Pending &p);  // free a slot without an answer (ends a subscription)
    void forgetPending(bool discovery);  // a finished job's commands still in flight
    void handleAuth(std::string_view text);
    void handleMessage(std::string text);
    void failAll(ErrorCode error);
    void expire();
    // Discovery (integration task).
    void discoveryResult(bool success, std::string text);
    void parseStep();
    void parseRegistryEntry(std::string_view element);
    void parseStateEntry(std::string_view entityId, std::string_view element);
    void nextPhase();
    void finishDiscovery(ErrorCode result);
    // Entity details (integration task).
    void detailsResult(Kind kind, bool success, std::string text);
    void finishDetails(ErrorCode result);

    WebSocketTransport &transport_;
    Clock clock_;
    std::atomic<HaState> state_{HaState::NotConfigured};
    std::function<void()> yield_;

    std::mutex settingsMutex_;
    Settings next_;  // from configure(), guarded
    bool nextChanged_ = false;
    Settings settings_;  // in use (integration task)

    bool open_ = false;  // transport between connect() and onClosed()
    uint64_t nowMs_ = 0;
    uint64_t retryAtMs_ = 0;
    uint64_t retryDelayMs_ = 0;
    uint64_t authDeadlineMs_ = 0;
    uint64_t nextPingMs_ = 0;
    uint32_t pingId_ = 0;  // 0: no ping outstanding
    uint64_t pongDeadlineMs_ = 0;
    uint32_t nextId_ = 1;
    Pending pending_[kMaxPendingCommands];

    // Discovery in progress (integration task): phase_ != None.
    Phase phase_ = Phase::None;
    DiscoverDone discoverDone_;
    HaDiscovery discovery_;
    std::map<std::string, RegistryEntry, std::less<>> registry_;  // allowlisted, enabled
    uint64_t discoveryStartMs_ = 0;
    std::set<std::string, std::less<>> seen_;  // every allowlisted entity in states (for entityStale())
    std::string text_;      // the message being parsed
    json::Cursor cursor_;   // over its list
    bool parsing_ = false;

    // Entity details in progress (integration task).
    bool detailing_ = false;
    // Registry, state and services are in: only the optional translations
    // step is left, and any way it ends (but a cancel) finishes Ok.
    bool detailsGathered_ = false;
    DetailsDone detailsDone_;
    HaEntityDetails details_;

    // Stale entities (any task reads them).
    mutable std::mutex staleMutex_;
    // Every allowlisted entity in the latest completed discovery, plus any
    // whose details were fetched since (none yet: nothing is stale for
    // missing from it).
    std::optional<std::set<std::string, std::less<>>> discovered_;

    // The cross-task mailboxes.
    mutable std::mutex mailboxMutex_;
    Mailbox<HaDiscovery> discoveryBox_;
    Mailbox<HaEntityDetails> detailsBox_;
    std::string detailsEntityId_;  // what detailsBox_ asks for
};

}  // namespace hubbo::integrations
