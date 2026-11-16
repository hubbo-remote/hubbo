#pragma once

// A fake Home Assistant WebSocket server and the in-process transport that
// talks to it, so the HA client, the adapter, contract and e2e tests run
// without a real HA. Host-only. Speaks the subset the client uses (see
// FakeHaServer). Replies are delivered on the transport's next poll(), as the
// real transport delivers them on the integration task.

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "host_guard.h"
#include "hubbo/domain/config.h"
#include "hubbo/ha/capabilities.h"
#include "hubbo/integrations/websocket_transport.h"

namespace hubbo::sim {

struct FakeHaEntity {
    ha::EntityInfo info;
    std::string friendlyName;
    std::string deviceClass;  // the state's device_class, "" for none
    // Entity registry entry (registered = false: the entity has no entry).
    bool registered = true;
    std::string uniqueId;
    std::string platform = "demo";
    std::string name;
    bool disabled = false;
    // The state ("" = "off"; turn_on/turn_off calls set it), and captured
    // attributes (the showcase), served as is instead of ones built from
    // `info` ("" = build them).
    std::string state;
    std::string attributesJson;
    // Attributes over the built or captured ones, by name, as JSON values
    // (/sim/ha attr=; a media player's select_source sets "source").
    std::vector<std::pair<std::string, std::string>> attributes;
    // A media player's is_volume_muted: set by media_player.volume_mute
    // calls (and /sim/ha muted=). nullopt: as built (false) or captured.
    std::optional<bool> muted;
    // A cover's current_position (kFakeCoverPosition at reset), set by
    // cover.set_cover_position calls. nullopt: HA reports none (unknown), or
    // it isn't a cover.
    std::optional<int32_t> position;
};

class FakeHaServer {
public:
    // The fixture entities (fixtureHaEntities()), registered, with realistic
    // names, platforms (denonavr, sonos, webostv, xbox, hue) and attributes,
    // plus kYamlOnlyEntity, which has a state but no registry entry.
    // Answers what the client sends: list_for_display, entity_registry/get,
    // subscribe_entities (one compressed event) and unsubscribe_events,
    // get_services, call_service (media_player.volume_mute also applies
    // is_volume_muted to the entity's state, as a real player does, and
    // cover.set_cover_position a cover's current_position), ping.
    FakeHaServer();
    static constexpr const char *kYamlOnlyEntity = "switch.patio_fountain";

    std::string token = "fake-ha-token";
    bool reachable = true;       // false: connect() fails and an open link drops
    bool answerPings = true;     // false: pings get no pong (dead connection)
    bool answerCommands = true;  // false: commands get no result (timeouts)
    bool answerTranslations = true;  // false: frontend/get_translations gets no result
    bool rejectAuth = false;     // true: every token gets auth_invalid (auth_failed)
    // true: a connect() never completes (the client stays connecting) until
    // it's set back to false.
    bool stallConnect = false;
    // "domain.service" calls that HA refuses (success: false).
    std::set<std::string> failing;
    // Entities whose service calls HA answers only after this many ms (by
    // `clock`), as an integration that acts at once but answers after its
    // own timeout. The call takes effect at once. Cleared by reset().
    std::map<std::string, uint32_t> slow;
    // The time slow answers wait by (the hub's or a test's clock).
    std::function<uint64_t()> clock;
    // Extra registry and state filler entities (sensors), for big discovery
    // payloads.
    size_t fillerEntities = 0;

    // Showcase mode (hub-sim serve --showcase): the fixture entities plus a
    // real HA's (showcaseHaEntities(), showcase.h), whose states and
    // services it serves as captured. Kept across reset().
    void enableShowcase();

    void addEntity(FakeHaEntity entity);
    void removeEntity(std::string_view entityId);  // HA forgot it
    // An entity's state ("on", "off", ...); one attribute, `value` as JSON
    // ("0.5", "true", "\"Game\"") or else as text ("Game"); or how slowly HA
    // answers its calls (0: at once). False: the fake has no such entity.
    bool setState(std::string_view entityId, std::string state);
    bool setAttribute(std::string_view entityId, const std::string &name, const std::string &value);
    bool setSlow(std::string_view entityId, uint32_t ms);
    // Sets a media player's is_volume_muted, as a volume_mute call would.
    // False: no such entity, or it isn't a media player.
    bool setMuted(std::string_view entityId, bool muted);
    // Its current is_volume_muted (nullopt: no such player, or unknown).
    std::optional<bool> muted(std::string_view entityId) const;
    // Back to construction: the fixture entities, reachable, answering, no
    // failures, no filler, no lastCallJson(). Counters and logs are kept.
    void reset();
    std::vector<FakeHaEntity> &entities() { return entities_; }
    const std::vector<FakeHaEntity> &entities() const { return entities_; }

    // What HA was asked to do, answered or not: "light.turn_on light.family_room brightness=128".
    const std::vector<std::string> &calls() const { return calls_; }
    // The latest call_service received, answered or not:
    // {"service":"media_player.volume_mute","entity_id":"media_player.denon",
    // "data":{"is_volume_muted":false}}, or "null" if none since reset().
    const std::string &lastCallJson() const { return lastCall_; }
    int connections() const { return connections_; }
    int authAttempts() const { return authAttempts_; }
    // Every message received, with any access token replaced by "<redacted>".
    const std::vector<std::string> &received() const { return received_; }
    // Open subscribe_entities subscriptions (the client should end each one).
    size_t subscriptions() const { return subscriptions_.size(); }

    // Server side of one connection (FakeHaTransport): the replies to send,
    // and the slow answers that are due (a new connection drops the rest).
    std::vector<std::string> onOpen();
    std::vector<std::string> onMessage(const std::string &text);
    std::vector<std::string> takeDue();

    // get_services' result, as entity details receive it.
    std::string servicesJson() const;
    // frontend/get_translations' resources for `integrations` (category
    // services), filtered by integration as HA does: in the showcase the
    // captured real strings (realHaTranslations()), else the real ones for
    // the default services only, inline.
    std::string translationsJson(const std::vector<std::string> &integrations) const;

private:
    // subscribe_entities' first event, "a": entity id -> compressed state
    // ({"s","a","c","lc"}); only `ids` if any are given.
    std::string compressedStates(const std::vector<std::string> &ids) const;
    std::string displayJson() const;                           // list_for_display's result
    std::string registryEntry(const FakeHaEntity &e) const;    // entity_registry/get's result
    FakeHaEntity *find(std::string_view entityId);

    std::vector<FakeHaEntity> entities_;
    std::vector<std::string> calls_;
    std::string lastCall_ = "null";
    std::vector<std::string> received_;
    int connections_ = 0;
    int authAttempts_ = 0;
    bool authed_ = false;
    bool showcase_ = false;
    std::set<uint32_t> subscriptions_;
    struct Delayed {
        uint64_t dueMs;
        std::string text;
    };
    std::vector<Delayed> delayed_;  // slow answers, oldest first
};

// Gives every home_assistant device in `config` without a snapshot the one
// registration would store (ha::makeSnapshot over the server's entity and
// services); devices whose entity the server doesn't have get none.
void addFakeHaSnapshots(ConfigSpec *config, const FakeHaServer &server);

class FakeHaTransport : public integrations::WebSocketTransport {
public:
    explicit FakeHaTransport(FakeHaServer &server) : server_(server) {}

    void setListener(Listener *listener) override { listener_ = listener; }
    bool connect(const std::string &url, const integrations::ConnectOptions &options) override;
    bool send(std::string_view text) override;
    void close() override;
    void poll(uint32_t timeoutMs) override;

    // Test hooks: drop the connection as a network failure would; the next
    // message over `maxMessageBytes` closes it (the firmware's 4 MB cap).
    void dropConnection();
    size_t maxMessageBytes = 4 * 1024 * 1024;
    int connectAttempts = 0;
    const std::string &lastUrl() const { return url_; }
    integrations::ConnectOptions lastOptions;  // from the latest connect()
    // Runs inside poll() just before onConnected() (a test advances its
    // clock here: a slow TLS connect).
    std::function<void()> beforeConnected;
    // Events delivered per poll() (default: all that are queued); 1 spreads
    // a handshake over several polls, as a real network does.
    size_t maxEventsPerPoll = SIZE_MAX;

private:
    enum class Event { Connected, Text, Closed };
    void openLink();  // queue Connected and the server's greeting
    struct Queued {
        Event event;
        std::string text;
    };

    FakeHaServer &server_;
    Listener *listener_ = nullptr;
    std::deque<Queued> events_;
    std::string url_;
    bool open_ = false;     // between connect() and the Closed event
    bool closing_ = false;  // a Closed event is queued
    bool stalled_ = false;  // connected, held back while server_.stallConnect
};

}  // namespace hubbo::sim
