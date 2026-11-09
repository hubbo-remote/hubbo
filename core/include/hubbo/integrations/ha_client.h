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
// section 1): the auth handshake, numbered commands (get_states,
// get_services, config/entity_registry/list, call_service), liveness pings
// and reconnects with backoff.
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
// Discovery never parses a big message in one go (a 2 MB get_states would
// hold the task for seconds): the result array is walked element by
// element, at most kParseStepEntities per poll(), so the integration task
// yields between steps (hasWork() tells its loop not to sleep meanwhile).
// The registry is fetched first and only allowlisted domains are kept;
// states are then read only for those entities.
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
    std::string uniqueId;  // entity registry; "" if the entity isn't in the registry
    std::string name;      // registry name, else original_name, else friendly_name, else ""
    std::string domain;    // "media_player"
    std::string platform;  // the integration behind it ("sonos"); registry, else ""
    ha::EntityInfo info;   // supported_features and the attributes that shape params
};

struct HaDiscovery {
    std::vector<HaCandidate> candidates;  // allowlisted domains only, sorted by entity_id
    size_t skipped = 0;                   // disabled in the registry
    // Allowlisted entities left out because kMaxCandidates were already
    // found (0: the list is complete). Shown, never silent.
    size_t truncated = 0;
    uint32_t elapsedMs = 0;  // request to result, parse steps and yields included (for logs)
    // The mailbox request this answers (discoveryGeneration() when it was
    // requested); 0 from discover().
    uint32_t generation = 0;
    // get_services' result as HA sent it (JSON object, domain -> services),
    // for integration-specific services (ha/). Not interpreted here. Can be
    // a few hundred KB (large allocations go to PSRAM on the board); free it
    // once normalized.
    std::string services;
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

    // Integration task: fetch the registry, states and services and build
    // the candidates. Ok (started; `done` is called exactly once: Ok, or
    // AdapterUnavailable / AdapterTimeout / AdapterFailed / MalformedJson),
    // Busy (one is running) or AdapterUnavailable (not Connected).
    using DiscoverDone = std::function<void(ErrorCode result, HaDiscovery discovery)>;
    ErrorCode discover(DiscoverDone done);

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
    // Any task. Bumped by requestDiscovery(), cancelDiscovery() (of a
    // running one), configure() with changed settings and reconnect(); a
    // result from an older generation is never published (a finished one
    // still in the mailbox is thrown away). A cache of candidates is current
    // while its HaDiscovery::generation equals this.
    uint32_t discoveryGeneration() const;

private:
    enum class Kind : uint8_t { CallService, EntityRegistry, GetStates, GetServices };
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
        std::string uniqueId;
        std::string platform;
        std::string name;
    };
    enum class Phase : uint8_t { None, Registry, States, Services };

    // WebSocketTransport::Listener
    void onConnected() override;
    void onText(std::string text) override;
    void onClosed() override;

    void setState(HaState state) { state_.store(state, std::memory_order_release); }
    void readClock() {
        if (clock_) nowMs_ = clock_();
    }
    void applySettings();
    void forgetDiscovery();  // settings changed: bump the generation, reset the mailbox
    void scheduleRetry();
    void failInFlight();  // the connection is gone: fail calls and discovery
    // `body`: the command's JSON object without "id" (e.g. {"type":"get_states"}).
    ErrorCode sendCommand(Kind kind, const std::string &body, uint32_t timeoutMs, CallDone done);
    void handleAuth(std::string_view text);
    void handleMessage(std::string text);
    void failAll(ErrorCode error);
    void expire();
    // Discovery (integration task).
    void discoveryResult(Kind kind, bool success, std::string text, json::Span result);
    void parseStep();
    void parseRegistryEntry(std::string_view element);
    void parseStateEntry(std::string_view element);
    void nextPhase();
    void finishDiscovery(ErrorCode result);

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

    // Discovery in progress (integration task).
    bool discovering_ = false;
    Phase phase_ = Phase::None;
    DiscoverDone discoverDone_;
    HaDiscovery discovery_;
    std::map<std::string, RegistryEntry, std::less<>> registry_;  // allowlisted, enabled
    uint64_t discoveryStartMs_ = 0;
    std::set<std::string, std::less<>> seen_;  // every allowlisted entity in states (for entityStale())
    std::string text_;            // the message being parsed
    json::ArrayCursor cursor_;    // over its result array
    bool parsing_ = false;

    // Stale entities (any task reads them).
    mutable std::mutex staleMutex_;
    // Every allowlisted entity in the latest completed discovery (none yet:
    // nothing is stale for missing from it).
    std::optional<std::set<std::string, std::less<>>> discovered_;

    // The cross-task discovery mailbox.
    mutable std::mutex mailboxMutex_;
    bool requested_ = false;
    bool cancelRequested_ = false;
    uint32_t generation_ = 0;
    DiscoveryStatus mailboxStatus_ = DiscoveryStatus::Idle;
    HaDiscovery mailboxResult_;
    ErrorCode mailboxError_ = ErrorCode::Ok;
};

}  // namespace hubbo::integrations
