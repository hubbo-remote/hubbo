#pragma once

// A fake Home Assistant WebSocket server and the in-process transport that
// talks to it, so the HA client, the adapter, contract and e2e tests run
// without a real HA. Host-only. Speaks the subset the client uses: the auth
// handshake, ping/pong, call_service, get_states, get_services and
// config/entity_registry/list.
//
// As real HA, a call_service for an entity it doesn't have (removed from
// entities()) still answers success. Replies are delivered on the transport's next poll() (as the real
// transport delivers them on the integration task).

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
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
    // Entity registry entry (registered = false: the entity has no entry).
    bool registered = true;
    std::string uniqueId;
    std::string platform = "demo";
    std::string name;
    bool disabled = false;
};

class FakeHaServer {
public:
    // The fixture entities (fixtureEntities()), registered, platform "demo".
    FakeHaServer();

    std::string token = "fake-ha-token";
    bool reachable = true;       // false: connect() fails and an open link drops
    bool answerPings = true;     // false: pings get no pong (dead connection)
    bool answerCommands = true;  // false: commands get no result (timeouts)
    // "domain.service" calls that HA refuses (success: false).
    std::set<std::string> failing;
    // Extra registry-only and state-only filler entities, for big discovery
    // payloads (e.g. hundreds of sensors).
    size_t fillerEntities = 0;

    void addEntity(FakeHaEntity entity);
    void removeEntity(std::string_view entityId);  // HA forgot it
    // Back to construction: the fixture entities, reachable, answering, no
    // failures, no filler. Counters and logs are kept.
    void reset();
    std::vector<FakeHaEntity> &entities() { return entities_; }
    const std::vector<FakeHaEntity> &entities() const { return entities_; }

    // What HA was asked to do, answered or not: "light.turn_on light.family_room brightness=128".
    const std::vector<std::string> &calls() const { return calls_; }
    int connections() const { return connections_; }
    int authAttempts() const { return authAttempts_; }
    // Every message received, with any access token replaced by "<redacted>".
    const std::vector<std::string> &received() const { return received_; }

    // Server side of one connection (FakeHaTransport): the replies to send.
    std::vector<std::string> onOpen();
    std::vector<std::string> onMessage(const std::string &text);

    // get_services' result, as discovery receives it.
    std::string servicesJson() const;

private:
    std::string statesJson() const;
    std::string registryJson() const;

    std::vector<FakeHaEntity> entities_;
    std::vector<std::string> calls_;
    std::vector<std::string> received_;
    int connections_ = 0;
    int authAttempts_ = 0;
    bool authed_ = false;
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
};

}  // namespace hubbo::sim
