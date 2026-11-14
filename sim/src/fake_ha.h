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
#include <set>
#include <string>
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
    // The state ("" = "on" for a media player, else "off"), and captured
    // attributes (the showcase), served as is instead of ones built from
    // `info` ("" = build them).
    std::string state;
    std::string attributesJson;
    // A media player's is_volume_muted: set by media_player.volume_mute
    // calls (and /sim/ha muted=). nullopt: as built (false) or captured.
    std::optional<bool> muted;
};

class FakeHaServer {
public:
    // The fixture entities (fixtureHaEntities()), registered, with realistic
    // names, platforms (denonavr, sonos, webostv, xbox, hue) and attributes,
    // plus kYamlOnlyEntity, which has a state but no registry entry.
    // Answers what the client sends: list_for_display, entity_registry/get,
    // subscribe_entities (one compressed event) and unsubscribe_events,
    // get_services, call_service (media_player.volume_mute also applies
    // is_volume_muted to the entity's state, as a real player does), ping.
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
    // Extra registry and state filler entities (sensors), for big discovery
    // payloads.
    size_t fillerEntities = 0;

    // Showcase mode (hub-sim serve --showcase): the fixture entities plus a
    // real HA's (showcaseHaEntities(), showcase.h), whose states and
    // services it serves as captured. Kept across reset().
    void enableShowcase();

    void addEntity(FakeHaEntity entity);
    void removeEntity(std::string_view entityId);  // HA forgot it
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

    // Server side of one connection (FakeHaTransport): the replies to send.
    std::vector<std::string> onOpen();
    std::vector<std::string> onMessage(const std::string &text);

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

    std::vector<FakeHaEntity> entities_;
    std::vector<std::string> calls_;
    std::string lastCall_ = "null";
    std::vector<std::string> received_;
    int connections_ = 0;
    int authAttempts_ = 0;
    bool authed_ = false;
    bool showcase_ = false;
    std::set<uint32_t> subscriptions_;
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
