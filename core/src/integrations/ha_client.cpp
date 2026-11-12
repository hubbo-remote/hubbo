#include "hubbo/integrations/ha_client.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace hubbo::integrations {

namespace {

bool supportedDomain(std::string_view entityId) {
    return ha::entityDomain(entityId) != ha::EntityDomain::Unsupported;
}

std::string domainOf(std::string_view entityId) {
    const size_t dot = entityId.find('.');
    return std::string(dot == std::string_view::npos ? entityId : entityId.substr(0, dot));
}

std::string serialize(const JsonDocument &doc) {
    std::string out;
    serializeJson(doc, out);
    return out;
}

std::string stringOr(JsonVariantConst v) { return v | ""; }

void stringList(JsonVariantConst v, std::vector<std::string> *out) {
    if (!v.is<JsonArrayConst>()) return;
    for (JsonVariantConst item : v.as<JsonArrayConst>()) {
        if (item.is<const char *>()) out->emplace_back(item.as<const char *>());
    }
}

int32_t intOr(JsonVariantConst v) {
    if (v.is<int32_t>()) return v.as<int32_t>();
    if (v.is<double>()) return static_cast<int32_t>(v.as<double>());
    return 0;
}

}  // namespace

const char *haStateName(HaState state) {
    switch (state) {
        case HaState::NotConfigured: return "not_configured";
        case HaState::Connecting: return "connecting";
        case HaState::Authenticating: return "authenticating";
        case HaState::Connected: return "connected";
        case HaState::AuthFailed: return "auth_failed";
        case HaState::Unreachable: return "unreachable";
    }
    return "unreachable";
}

const char *discoveryStatusName(DiscoveryStatus status) {
    switch (status) {
        case DiscoveryStatus::Idle: return "idle";
        case DiscoveryStatus::Running: return "running";
        case DiscoveryStatus::Done: return "done";
        case DiscoveryStatus::Failed: return "failed";
    }
    return "failed";
}

HomeAssistantClient::HomeAssistantClient(WebSocketTransport &transport, Clock clock)
    : transport_(transport), clock_(std::move(clock)) {
    transport_.setListener(this);
}

HomeAssistantClient::~HomeAssistantClient() { transport_.setListener(nullptr); }

// ---- settings and connection --------------------------------------------

void HomeAssistantClient::configure(std::string url, std::string token, bool verifyTls) {
    std::lock_guard<std::mutex> lock(settingsMutex_);
    if (url == next_.url && token == next_.token && verifyTls == next_.verifyTls) return;
    next_.url = std::move(url);
    next_.token = std::move(token);
    next_.verifyTls = verifyTls;
    nextChanged_ = true;
    forgetRequests();
}

void HomeAssistantClient::reconnect() {
    std::lock_guard<std::mutex> lock(settingsMutex_);
    nextChanged_ = true;
    forgetRequests();
}

void HomeAssistantClient::forgetRequests() {
    // Results from the old settings (maybe another HA) are never published:
    // a running job is dropped when it ends (failInFlight() in
    // applySettings()), a finished one is thrown away now.
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    discoveryBox_.forget();
    detailsBox_.forget();
}

uint32_t HomeAssistantClient::discoveryGeneration() const {
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    return discoveryBox_.generation;
}

void HomeAssistantClient::applySettings() {
    {
        std::lock_guard<std::mutex> lock(settingsMutex_);
        if (!nextChanged_) return;
        settings_ = next_;
        nextChanged_ = false;
    }
    // New settings (maybe another HA): reconnect at once, not after the
    // backoff, and forget what we knew about its entities. The state is set
    // before the close arrives, so onClosed() knows it wasn't a drop.
    if (open_) transport_.close();  // onClosed() follows
    failInFlight();
    retryDelayMs_ = 0;
    retryAtMs_ = nowMs_;
    setState(settings_.url.empty() || settings_.token.empty() ? HaState::NotConfigured : HaState::Unreachable);
    std::lock_guard<std::mutex> lock(staleMutex_);
    discovered_.reset();
}

void HomeAssistantClient::failInFlight() {
    failAll(ErrorCode::AdapterUnavailable);
    if (phase_ != Phase::None) finishDiscovery(ErrorCode::AdapterUnavailable);
    if (detailing_) finishDetails(ErrorCode::AdapterUnavailable);
    pingId_ = 0;
}

void HomeAssistantClient::scheduleRetry() {
    retryDelayMs_ = retryDelayMs_ == 0 ? kFirstRetryMs : std::min<uint64_t>(retryDelayMs_ * 2, kMaxRetryMs);
    retryAtMs_ = nowMs_ + retryDelayMs_;
}

void HomeAssistantClient::poll(uint64_t nowMs) {
    nowMs_ = nowMs;
    readClock();
    applySettings();
    const HaState s = state();

    if (s == HaState::Unreachable && !open_ && nowMs_ >= retryAtMs_) {
        ConnectOptions options;
        options.verifyTls = settings_.verifyTls;
        if (transport_.connect(settings_.url, options)) {
            open_ = true;
            setState(HaState::Connecting);
        } else {
            scheduleRetry();
        }
    }
    if (s == HaState::Authenticating && nowMs_ >= authDeadlineMs_) transport_.close();
    if (s == HaState::Connected) {
        if (pingId_ != 0 && nowMs_ >= pongDeadlineMs_) {
            transport_.close();  // no pong: the connection is dead
        } else if (pingId_ == 0 && nowMs_ >= nextPingMs_) {
            const uint32_t id = nextId_++;
            if (transport_.send("{\"id\":" + std::to_string(id) + ",\"type\":\"ping\"}")) {
                pingId_ = id;
                pongDeadlineMs_ = nowMs_ + kPongTimeoutMs;
            } else {
                transport_.close();
            }
        }
    }
    expire();

    serviceMailboxes();
    if (parsing_) parseStep();
}

void HomeAssistantClient::serviceMailboxes() {
    // Cancels first, so a cancel followed by a new request starts the new one.
    bool cancelDiscovery = false;
    bool cancelDetails = false;
    {
        std::lock_guard<std::mutex> lock(mailboxMutex_);
        std::swap(cancelDiscovery, discoveryBox_.cancelRequested);
        std::swap(cancelDetails, detailsBox_.cancelRequested);
    }
    if (cancelDiscovery) finishDiscovery(ErrorCode::Cancelled);
    if (cancelDetails) finishDetails(ErrorCode::Cancelled);

    // A cancel or new settings since a request bumped its generation (and
    // reset its mailbox): that result is dropped by publish().
    bool startDiscovery = false;
    bool startDetails = false;
    uint32_t discoveryGeneration = 0;
    uint32_t detailsGeneration = 0;
    std::string entityId;
    {
        std::lock_guard<std::mutex> lock(mailboxMutex_);
        // One of each at a time (a direct discover() may be running): a
        // request waits for it.
        if (phase_ == Phase::None) std::swap(startDiscovery, discoveryBox_.requested);
        if (!detailing_) std::swap(startDetails, detailsBox_.requested);
        discoveryGeneration = discoveryBox_.generation;
        detailsGeneration = detailsBox_.generation;
        entityId = detailsEntityId_;
    }
    if (startDiscovery) {
        const auto publish = [this, discoveryGeneration](ErrorCode result, HaDiscovery discovery) {
            discovery.generation = discoveryGeneration;
            std::lock_guard<std::mutex> lock(mailboxMutex_);
            discoveryBox_.publish(discoveryGeneration, result, std::move(discovery));
        };
        const ErrorCode started = discover(publish);
        if (started != ErrorCode::Ok) publish(started, HaDiscovery());
    }
    if (startDetails) {
        const auto publish = [this, detailsGeneration](ErrorCode result, HaEntityDetails details) {
            std::lock_guard<std::mutex> lock(mailboxMutex_);
            detailsBox_.publish(detailsGeneration, result, std::move(details));
        };
        const ErrorCode started = entityDetails(entityId, publish);
        if (started != ErrorCode::Ok) publish(started, HaEntityDetails());
    }
}

bool HomeAssistantClient::hasWork() const {
    if (parsing_) return true;
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    return discoveryBox_.requested || detailsBox_.requested;
}

void HomeAssistantClient::onConnected() {
    readClock();  // the connect may have taken seconds inside transport.poll()
    setState(HaState::Authenticating);
    authDeadlineMs_ = nowMs_ + kAuthTimeoutMs;
}

void HomeAssistantClient::onClosed() {
    readClock();
    open_ = false;
    failInFlight();
    // Only a link that dropped by itself backs off. Closes we caused set the
    // state first: AuthFailed (no retry), NotConfigured, or Unreachable with
    // an immediate retry (new settings).
    const HaState s = state();
    if (s == HaState::Connecting || s == HaState::Authenticating || s == HaState::Connected) {
        setState(HaState::Unreachable);
        scheduleRetry();
    }
}

void HomeAssistantClient::onText(std::string text) {
    readClock();
    if (state() == HaState::Authenticating) {
        handleAuth(text);
    } else if (state() == HaState::Connected) {
        handleMessage(std::move(text));
    }
}

void HomeAssistantClient::handleAuth(std::string_view text) {
    JsonDocument doc;
    if (deserializeJson(doc, text.data(), text.size())) return;
    const std::string type = stringOr(doc["type"]);
    if (type == "auth_required") {
        JsonDocument auth;
        auth["type"] = "auth";
        auth["access_token"] = settings_.token;  // the only place the token is used
        if (!transport_.send(serialize(auth))) transport_.close();
    } else if (type == "auth_ok") {
        setState(HaState::Connected);
        retryDelayMs_ = 0;
        nextPingMs_ = nowMs_ + kPingIntervalMs;
    } else if (type == "auth_invalid") {
        setState(HaState::AuthFailed);  // set first: onClosed() then doesn't retry
        transport_.close();
    }
}

// ---- commands ------------------------------------------------------------

ErrorCode HomeAssistantClient::sendCommand(Kind kind, const std::string &body, uint32_t timeoutMs, CallDone done) {
    if (state() != HaState::Connected) return ErrorCode::AdapterUnavailable;
    Pending *slot = nullptr;
    for (auto &p : pending_) {
        if (!p.used) {
            slot = &p;
            break;
        }
    }
    if (slot == nullptr) return ErrorCode::QueueFull;
    const uint32_t id = nextId_++;
    // body is "{...}": splice the id in front.
    const std::string message = "{\"id\":" + std::to_string(id) + (body.size() > 2 ? "," : "") + body.substr(1);
    if (!transport_.send(message)) return ErrorCode::AdapterUnavailable;
    slot->used = true;
    slot->id = id;
    slot->kind = kind;
    slot->deadlineMs = nowMs_ + timeoutMs;
    slot->done = std::move(done);
    return ErrorCode::Ok;
}

ErrorCode HomeAssistantClient::callService(const ha::ServiceCall &call, CallDone done) {
    JsonDocument doc;
    doc["type"] = "call_service";
    doc["domain"] = call.domain;
    doc["service"] = call.service;
    doc["target"]["entity_id"] = call.entityId;
    JsonObject data = doc["service_data"].to<JsonObject>();
    for (const ha::ServiceField &field : call.data) {
        switch (field.value.kind) {
            case ha::ServiceValue::Kind::Bool: data[field.key] = field.value.boolean; break;
            case ha::ServiceValue::Kind::Int: data[field.key] = field.value.integer; break;
            case ha::ServiceValue::Kind::Number: data[field.key] = field.value.number; break;
            case ha::ServiceValue::Kind::String: data[field.key] = field.value.string; break;
        }
    }
    return sendCommand(Kind::CallService, serialize(doc), kCommandTimeoutMs, std::move(done));
}

bool HomeAssistantClient::entityStale(std::string_view entityId) const {
    std::lock_guard<std::mutex> lock(staleMutex_);
    return discovered_ && discovered_->find(entityId) == discovered_->end();
}

void HomeAssistantClient::failAll(ErrorCode error) {
    for (auto &p : pending_) {
        if (!p.used) continue;
        p.used = false;
        CallDone done = std::move(p.done);
        if (done) done(error);
    }
}

void HomeAssistantClient::expire() {
    for (auto &p : pending_) {
        if (!p.used || nowMs_ < p.deadlineMs) continue;
        release(p);
        CallDone done = std::move(p.done);
        if (p.kind == Kind::CallService) {
            if (done) done(ErrorCode::AdapterTimeout);
        } else if (isDiscovery(p.kind)) {
            finishDiscovery(ErrorCode::AdapterTimeout);
        } else {
            finishDetails(ErrorCode::AdapterTimeout);
        }
    }
}

void HomeAssistantClient::unsubscribe(uint32_t subscriptionId) {
    const uint32_t id = nextId_++;  // its result is ignored (unknown id)
    transport_.send("{\"id\":" + std::to_string(id) + ",\"type\":\"unsubscribe_events\",\"subscription\":" +
                    std::to_string(subscriptionId) + "}");
}

void HomeAssistantClient::release(Pending &p) {
    p.used = false;
    if (isSubscription(p.kind) && state() == HaState::Connected) unsubscribe(p.id);  // HA stops sending
}

void HomeAssistantClient::forgetPending(bool discovery) {
    // A job ended (or was cancelled) with commands still in flight: their
    // results are ignored, and a subscription is ended so HA stops sending.
    for (auto &p : pending_) {
        if (p.used && p.kind != Kind::CallService && isDiscovery(p.kind) == discovery) release(p);
    }
}

void HomeAssistantClient::handleMessage(std::string text) {
    // Only the structure is scanned here: a discovery message can be MBs.
    const std::string_view view(text);
    const json::Span idSpan = json::member(view, "id");
    const json::Span typeSpan = json::member(view, "type");
    if (!idSpan.found() || !typeSpan.found()) return;
    const uint32_t id = static_cast<uint32_t>(std::strtoul(std::string(idSpan.in(view)).c_str(), nullptr, 10));
    const std::string_view type = typeSpan.in(view);
    if (type == "\"pong\"") {
        if (id == pingId_) {
            pingId_ = 0;
            nextPingMs_ = nowMs_ + kPingIntervalMs;
        }
        return;
    }
    const bool event = type == "\"event\"";
    if (!event && type != "\"result\"") return;
    const bool success = event || json::member(view, "success").in(view) == "true";
    for (auto &p : pending_) {
        if (!p.used || p.id != id) continue;
        // A subscription's result comes first; its first event carries the data.
        if (isSubscription(p.kind) && !event && success) return;
        if (event && !isSubscription(p.kind)) return;
        p.used = false;
        if (event) unsubscribe(id);  // one snapshot is all we want
        if (p.kind == Kind::CallService) {
            CallDone done = std::move(p.done);
            if (done) done(success ? ErrorCode::Ok : ErrorCode::AdapterFailed);
        } else if (isDiscovery(p.kind)) {
            discoveryResult(success, std::move(text));
        } else {
            detailsResult(p.kind, success, std::move(text));
        }
        return;
    }
    // Unknown id (timed out already, or a subscription we ended): ignored.
}

namespace {

// A JSON member's string value (without quotes), or "". Cheap: no parse.
// Fine for the ids HA uses as keys and values here (no escapes).
std::string_view stringMember(std::string_view element, std::string_view key) {
    const std::string_view quoted = json::member(element, key).in(element);
    return quoted.size() < 2 || quoted.front() != '"' ? std::string_view() : quoted.substr(1, quoted.size() - 2);
}

// An entity's EntityInfo from its compressed-state attributes ("a").
ha::EntityInfo entityInfo(const std::string &entityId, JsonVariantConst attrs) {
    ha::EntityInfo info;
    info.entityId = entityId;
    info.supportedFeatures = static_cast<uint32_t>(intOr(attrs["supported_features"]));
    stringList(attrs["source_list"], &info.sourceList);
    stringList(attrs["sound_mode_list"], &info.soundModeList);
    stringList(attrs["supported_color_modes"], &info.supportedColorModes);
    info.minColorTempKelvin = intOr(attrs["min_color_temp_kelvin"]);
    info.maxColorTempKelvin = intOr(attrs["max_color_temp_kelvin"]);
    stringList(attrs["preset_modes"], &info.presetModes);
    info.percentageStep = attrs["percentage_step"] | 0.0;
    return info;
}

}  // namespace

// ---- discovery -------------------------------------------------------------

ErrorCode HomeAssistantClient::discover(DiscoverDone done) {
    if (phase_ != Phase::None) return ErrorCode::Busy;
    if (state() != HaState::Connected) return ErrorCode::AdapterUnavailable;
    const ErrorCode sent = sendCommand(Kind::Registry, "{\"type\":\"config/entity_registry/list_for_display\"}",
                                       kDiscoveryTimeoutMs, nullptr);
    if (sent != ErrorCode::Ok) return sent;
    phase_ = Phase::Registry;
    discoverDone_ = std::move(done);
    discovery_ = HaDiscovery();
    registry_.clear();
    seen_.clear();
    discoveryStartMs_ = nowMs_;
    return ErrorCode::Ok;
}

void HomeAssistantClient::discoveryResult(bool success, std::string text) {
    if (!success) return finishDiscovery(ErrorCode::AdapterFailed);
    text_ = std::move(text);
    // Straight to the list (list_for_display's "entities", the event's "a"):
    // the big value is scanned once, by the walk.
    const bool registry = phase_ == Phase::Registry;
    const size_t list =
        json::valueStart(text_, json::valueStart(text_, 0, registry ? "result" : "event"), registry ? "entities" : "a");
    cursor_ = json::Cursor(text_, list);
    if (!cursor_.ok()) return finishDiscovery(ErrorCode::MalformedJson);
    parsing_ = true;  // parsed in steps by poll()
}

void HomeAssistantClient::parseStep() {
    std::string_view element;
    std::string_view key;
    for (size_t n = 0; n < kParseStepEntities; n++) {
        if (!cursor_.next(text_, &element, &key)) {
            parsing_ = false;
            if (!cursor_.ok()) return finishDiscovery(ErrorCode::MalformedJson);
            return nextPhase();
        }
        if (phase_ == Phase::Registry) {
            parseRegistryEntry(element);
        } else {
            parseStateEntry(key, element);
        }
    }
    if (yield_) yield_();
}

void HomeAssistantClient::parseRegistryEntry(std::string_view element) {
    // Prefilter on the id: most of a registry is unsupported domains. Only
    // enabled entities are listed.
    const std::string_view id = stringMember(element, "ei");
    if (!supportedDomain(id)) return;
    JsonDocument doc;
    if (deserializeJson(doc, element.data(), element.size())) return;  // skip a malformed entry
    RegistryEntry entry;
    entry.platform = stringOr(doc["pl"]);
    entry.name = stringOr(doc["en"]);
    registry_[std::string(id)] = std::move(entry);
}

void HomeAssistantClient::parseStateEntry(std::string_view id, std::string_view element) {
    // Cheap prefilter: the entity id decides whether this element matters.
    if (!supportedDomain(id)) return;
    const std::string entityId(id);
    seen_.insert(entityId);
    if (discovery_.candidates.size() >= kMaxCandidates) {
        discovery_.truncated++;
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, element.data(), element.size())) return;
    JsonVariantConst attrs = doc["a"];
    HaCandidate c;
    c.entityId = entityId;
    c.domain = domainOf(entityId);
    c.state = stringOr(doc["s"]);
    c.info = entityInfo(entityId, attrs);
    c.name = stringOr(attrs["friendly_name"]);
    // An entity without a registry entry (YAML-only) is still offered.
    const auto reg = registry_.find(entityId);
    if (reg != registry_.end()) {
        c.platform = reg->second.platform;
        if (c.name.empty()) c.name = reg->second.name;
    }
    discovery_.candidates.push_back(std::move(c));
}

void HomeAssistantClient::nextPhase() {
    text_ = std::string();  // free the big message before asking for the next
    cursor_ = json::Cursor();
    if (phase_ == Phase::States) return finishDiscovery(ErrorCode::Ok);
    phase_ = Phase::States;
    const ErrorCode sent = sendCommand(Kind::States, "{\"type\":\"subscribe_entities\"}", kDiscoveryTimeoutMs, nullptr);
    if (sent != ErrorCode::Ok) finishDiscovery(sent);
}

void HomeAssistantClient::finishDiscovery(ErrorCode result) {
    if (phase_ == Phase::None) return;
    parsing_ = false;
    phase_ = Phase::None;
    text_ = std::string();
    cursor_ = json::Cursor();
    registry_.clear();
    forgetPending(true);
    HaDiscovery out;
    if (result == ErrorCode::Ok) {
        out = std::move(discovery_);
        std::sort(out.candidates.begin(), out.candidates.end(),
                  [](const HaCandidate &a, const HaCandidate &b) { return a.entityId < b.entityId; });
        out.elapsedMs = static_cast<uint32_t>(nowMs_ - discoveryStartMs_);
        std::lock_guard<std::mutex> lock(staleMutex_);
        discovered_ = std::move(seen_);  // all of HA's entities, also those over the cap
    }
    seen_ = {};
    discovery_ = HaDiscovery();
    DiscoverDone done = std::move(discoverDone_);
    discoverDone_ = nullptr;
    if (done) done(result, std::move(out));
}

// ---- entity details --------------------------------------------------------

ErrorCode HomeAssistantClient::entityDetails(const std::string &entityId, DetailsDone done) {
    if (detailing_) return ErrorCode::Busy;
    if (state() != HaState::Connected) return ErrorCode::AdapterUnavailable;
    if (!supportedDomain(entityId)) return ErrorCode::InvalidId;  // also an invalid id
    JsonDocument doc;
    doc["type"] = "config/entity_registry/get";
    doc["entity_id"] = entityId;
    const ErrorCode sent = sendCommand(Kind::DetailsRegistry, serialize(doc), kDiscoveryTimeoutMs, nullptr);
    if (sent != ErrorCode::Ok) return sent;
    detailing_ = true;
    detailsDone_ = std::move(done);
    details_ = HaEntityDetails();
    details_.entityId = entityId;
    return ErrorCode::Ok;
}

void HomeAssistantClient::detailsResult(Kind kind, bool success, std::string text) {
    const json::Span body = json::member(text, kind == Kind::DetailsState ? "event" : "result");
    // A registry lookup's not_found is an answer: YAML-only, no registry entry.
    const bool noEntry = !success && kind == Kind::DetailsRegistry &&
                         json::member(text, json::member(text, "error"), "code").in(text) == "\"not_found\"";
    if (kind == Kind::DetailsTranslations) {
        // Anything but a result object leaves the fallback labels.
        const json::Span resources = json::member(text, body, "resources");
        if (success && resources.found() && text[resources.begin] == '{') {
            text.erase(resources.end);
            text.erase(0, resources.begin);
            details_.translations = std::move(text);
        }
        return finishDetails(ErrorCode::Ok);
    }
    if (!success && !noEntry) return finishDetails(ErrorCode::AdapterFailed);
    ErrorCode sent = ErrorCode::Ok;
    if (kind == Kind::DetailsRegistry) {
        if (body.found()) {  // else YAML-only: no registry entry
            JsonDocument doc;
            const std::string_view entry = body.in(text);
            if (deserializeJson(doc, entry.data(), entry.size())) return finishDetails(ErrorCode::MalformedJson);
            details_.uniqueId = stringOr(doc["unique_id"]);
            details_.platform = stringOr(doc["platform"]);
            details_.name = stringOr(doc["name"]);
            if (details_.name.empty()) details_.name = stringOr(doc["original_name"]);
        }
        JsonDocument request;
        request["type"] = "subscribe_entities";
        request["entity_ids"].add(details_.entityId);
        sent = sendCommand(Kind::DetailsState, serialize(request), kDiscoveryTimeoutMs, nullptr);
    } else if (kind == Kind::DetailsState) {
        const json::Span state = json::member(text, json::member(text, body, "a"), details_.entityId);
        if (!state.found()) return finishDetails(ErrorCode::EntityNotFound);
        JsonDocument doc;
        const std::string_view element = state.in(text);
        if (deserializeJson(doc, element.data(), element.size())) return finishDetails(ErrorCode::MalformedJson);
        details_.info = entityInfo(details_.entityId, doc["a"]);
        const std::string friendly = stringOr(doc["a"]["friendly_name"]);
        if (!friendly.empty()) details_.name = friendly;
        sent = sendCommand(Kind::DetailsServices, "{\"type\":\"get_services\"}", kDiscoveryTimeoutMs, nullptr);
    } else {
        if (!body.found() || text[body.begin] != '{') return finishDetails(ErrorCode::MalformedJson);
        text.erase(body.end);  // keep only the result, without a second copy
        text.erase(0, body.begin);
        details_.services = std::move(text);
        if (details_.platform.empty()) return finishDetails(ErrorCode::Ok);  // no integration: none to label
        // Its integration's service names, as HA's UI shows them. Optional.
        detailsGathered_ = true;
        JsonDocument request;
        request["type"] = "frontend/get_translations";
        request["language"] = "en";
        request["category"] = "services";
        request["integration"].add(details_.platform);
        sent = sendCommand(Kind::DetailsTranslations, serialize(request), kCommandTimeoutMs, nullptr);
    }
    if (sent != ErrorCode::Ok) finishDetails(sent);
}

void HomeAssistantClient::finishDetails(ErrorCode result) {
    if (!detailing_) return;
    detailing_ = false;
    // A failed translations step (timeout, error, dropped connection) keeps
    // what was gathered, with the fallback labels.
    if (detailsGathered_ && result != ErrorCode::Cancelled) result = ErrorCode::Ok;
    detailsGathered_ = false;
    forgetPending(false);
    HaEntityDetails out;
    if (result == ErrorCode::Ok) {
        out = std::move(details_);
        // Proof HA has it: not stale until the next discovery says otherwise.
        std::lock_guard<std::mutex> lock(staleMutex_);
        if (discovered_) discovered_->insert(out.entityId);
    }
    details_ = HaEntityDetails();
    DetailsDone done = std::move(detailsDone_);
    detailsDone_ = nullptr;
    if (done) done(result, std::move(out));
}

// ---- cross-task mailboxes --------------------------------------------------

ErrorCode HomeAssistantClient::requestDiscovery() {
    if (state() != HaState::Connected) return ErrorCode::AdapterUnavailable;
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    return discoveryBox_.request();
}

void HomeAssistantClient::cancelDiscovery() {
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    discoveryBox_.cancel();
}

DiscoveryStatus HomeAssistantClient::takeDiscovery(HaDiscovery *out, ErrorCode *error) {
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    return discoveryBox_.take(out, error);
}

ErrorCode HomeAssistantClient::requestEntityDetails(std::string_view entityId) {
    if (state() != HaState::Connected) return ErrorCode::AdapterUnavailable;
    if (!supportedDomain(entityId)) return ErrorCode::InvalidId;  // also an invalid id
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    const ErrorCode result = detailsBox_.request();
    if (result == ErrorCode::Ok) detailsEntityId_.assign(entityId);
    return result;
}

void HomeAssistantClient::cancelEntityDetails() {
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    detailsBox_.cancel();
}

DiscoveryStatus HomeAssistantClient::takeEntityDetails(HaEntityDetails *out, ErrorCode *error) {
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    return detailsBox_.take(out, error);
}

}  // namespace hubbo::integrations
