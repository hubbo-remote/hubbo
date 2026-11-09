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
    forgetDiscovery();
}

void HomeAssistantClient::reconnect() {
    std::lock_guard<std::mutex> lock(settingsMutex_);
    nextChanged_ = true;
    forgetDiscovery();
}

void HomeAssistantClient::forgetDiscovery() {
    // Results from the old settings (maybe another HA) are never published:
    // the running one is dropped when it ends (failInFlight() in
    // applySettings()), a finished one is thrown away now.
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    generation_++;
    requested_ = false;
    mailboxStatus_ = DiscoveryStatus::Idle;
    mailboxResult_ = HaDiscovery();
}

uint32_t HomeAssistantClient::discoveryGeneration() const {
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    return generation_;
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
    if (discovering_) finishDiscovery(ErrorCode::AdapterUnavailable);
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

    bool cancel = false;
    {
        std::lock_guard<std::mutex> lock(mailboxMutex_);
        cancel = cancelRequested_;
        cancelRequested_ = false;
    }
    if (cancel) finishDiscovery(ErrorCode::Cancelled);

    if (parsing_) {
        parseStep();
    } else {
        bool start = false;
        uint32_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(mailboxMutex_);
            start = requested_;
            requested_ = false;
            generation = generation_;
        }
        if (start) {
            // A cancel or new settings since the request bumped the
            // generation (and reset the mailbox): that result is dropped.
            const auto publish = [this, generation](ErrorCode result, HaDiscovery discovery) {
                std::lock_guard<std::mutex> lock(mailboxMutex_);
                if (generation != generation_) return;
                mailboxStatus_ = result == ErrorCode::Ok ? DiscoveryStatus::Done : DiscoveryStatus::Failed;
                mailboxError_ = result;
                discovery.generation = generation;
                mailboxResult_ = std::move(discovery);
            };
            const ErrorCode started = discover(publish);
            if (started != ErrorCode::Ok) publish(started, HaDiscovery());
        }
    }
}

bool HomeAssistantClient::hasWork() const {
    if (parsing_) return true;
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    return requested_;
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
        p.used = false;
        CallDone done = std::move(p.done);
        if (done) {
            done(ErrorCode::AdapterTimeout);
        } else if (discovering_) {
            finishDiscovery(ErrorCode::AdapterTimeout);
        }
    }
}

void HomeAssistantClient::handleMessage(std::string text) {
    // Only the structure is scanned here: a discovery result can be MBs.
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
    if (type != "\"result\"") return;  // events: we don't subscribe
    const bool success = json::member(view, "success").in(view) == "true";
    for (auto &p : pending_) {
        if (!p.used || p.id != id) continue;
        p.used = false;
        if (p.kind == Kind::CallService) {
            CallDone done = std::move(p.done);
            if (done) done(success ? ErrorCode::Ok : ErrorCode::AdapterFailed);
        } else if (discovering_) {
            const json::Span result = json::member(view, "result");
            discoveryResult(p.kind, success, std::move(text), result);
        }
        return;
    }
    // Unknown id (timed out already): ignored.
}

// ---- discovery -------------------------------------------------------------

ErrorCode HomeAssistantClient::discover(DiscoverDone done) {
    if (discovering_) return ErrorCode::Busy;
    if (state() != HaState::Connected) return ErrorCode::AdapterUnavailable;
    const ErrorCode sent =
        sendCommand(Kind::EntityRegistry, "{\"type\":\"config/entity_registry/list\"}", kDiscoveryTimeoutMs, nullptr);
    if (sent != ErrorCode::Ok) return sent;
    discovering_ = true;
    phase_ = Phase::Registry;
    discoverDone_ = std::move(done);
    discovery_ = HaDiscovery();
    registry_.clear();
    seen_.clear();
    discoveryStartMs_ = nowMs_;
    return ErrorCode::Ok;
}

void HomeAssistantClient::discoveryResult(Kind kind, bool success, std::string text, json::Span result) {
    if (!success) return finishDiscovery(ErrorCode::AdapterFailed);
    if (kind == Kind::GetServices) {
        if (!result.found() || text[result.begin] != '{') return finishDiscovery(ErrorCode::MalformedJson);
        text.erase(result.end);  // keep only the result, without a second copy
        text.erase(0, result.begin);
        discovery_.services = std::move(text);
        return finishDiscovery(ErrorCode::Ok);
    }
    text_ = std::move(text);
    cursor_ = json::ArrayCursor(text_, result);
    if (!cursor_.ok()) return finishDiscovery(ErrorCode::MalformedJson);
    parsing_ = true;  // parsed in steps by poll()
}

void HomeAssistantClient::parseStep() {
    std::string_view element;
    for (size_t n = 0; n < kParseStepEntities; n++) {
        if (!cursor_.next(text_, &element)) {
            parsing_ = false;
            if (!cursor_.ok()) return finishDiscovery(ErrorCode::MalformedJson);
            return nextPhase();
        }
        if (phase_ == Phase::Registry) {
            parseRegistryEntry(element);
        } else {
            parseStateEntry(element);
        }
    }
    if (yield_) yield_();
}

namespace {

// The element's "entity_id" (without quotes) or "". Cheap: no parse.
std::string_view entityIdOf(std::string_view element) {
    const std::string_view quoted = json::member(element, "entity_id").in(element);
    return quoted.size() < 2 ? std::string_view() : quoted.substr(1, quoted.size() - 2);
}

}  // namespace

void HomeAssistantClient::parseRegistryEntry(std::string_view element) {
    // Prefilter on the id: most of a registry is unsupported domains.
    if (!supportedDomain(entityIdOf(element))) return;
    JsonDocument doc;
    if (deserializeJson(doc, element.data(), element.size())) return;  // skip a malformed entry
    const std::string entityId = stringOr(doc["entity_id"]);
    if (!doc["disabled_by"].isNull()) {
        discovery_.skipped++;
        return;
    }
    RegistryEntry entry;
    entry.uniqueId = stringOr(doc["unique_id"]);
    entry.platform = stringOr(doc["platform"]);
    entry.name = stringOr(doc["name"]);
    if (entry.name.empty()) entry.name = stringOr(doc["original_name"]);
    registry_[entityId] = std::move(entry);
}

void HomeAssistantClient::parseStateEntry(std::string_view element) {
    // Cheap prefilter: the entity id decides whether this element matters.
    const std::string_view id = entityIdOf(element);
    if (!supportedDomain(id)) return;
    const std::string entityId(id);
    // An entity without a registry entry is still offered (no unique_id).
    const auto reg = registry_.find(entityId);
    seen_.insert(entityId);
    if (discovery_.candidates.size() >= kMaxCandidates) {
        discovery_.truncated++;
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, element.data(), element.size())) return;
    JsonVariantConst attrs = doc["attributes"];
    HaCandidate c;
    c.entityId = entityId;
    c.domain = domainOf(entityId);
    c.info.entityId = entityId;
    c.info.supportedFeatures = static_cast<uint32_t>(intOr(attrs["supported_features"]));
    stringList(attrs["source_list"], &c.info.sourceList);
    stringList(attrs["sound_mode_list"], &c.info.soundModeList);
    stringList(attrs["supported_color_modes"], &c.info.supportedColorModes);
    c.info.minColorTempKelvin = intOr(attrs["min_color_temp_kelvin"]);
    c.info.maxColorTempKelvin = intOr(attrs["max_color_temp_kelvin"]);
    if (reg != registry_.end()) {
        c.uniqueId = reg->second.uniqueId;
        c.platform = reg->second.platform;
        c.name = reg->second.name;
    }
    if (c.name.empty()) c.name = stringOr(attrs["friendly_name"]);
    discovery_.candidates.push_back(std::move(c));
}

void HomeAssistantClient::nextPhase() {
    text_ = std::string();  // free the big message before asking for the next
    cursor_ = json::ArrayCursor();
    ErrorCode sent = ErrorCode::Ok;
    if (phase_ == Phase::Registry) {
        phase_ = Phase::States;
        sent = sendCommand(Kind::GetStates, "{\"type\":\"get_states\"}", kDiscoveryTimeoutMs, nullptr);
    } else {
        registry_.clear();
        phase_ = Phase::Services;
        sent = sendCommand(Kind::GetServices, "{\"type\":\"get_services\"}", kDiscoveryTimeoutMs, nullptr);
    }
    if (sent != ErrorCode::Ok) finishDiscovery(sent);
}

void HomeAssistantClient::finishDiscovery(ErrorCode result) {
    if (!discovering_) return;
    discovering_ = false;
    parsing_ = false;
    phase_ = Phase::None;
    text_ = std::string();
    cursor_ = json::ArrayCursor();
    registry_.clear();
    // A discovery command still in flight is forgotten (its result is ignored).
    for (auto &p : pending_) {
        if (p.used && p.kind != Kind::CallService) p.used = false;
    }
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

ErrorCode HomeAssistantClient::requestDiscovery() {
    if (state() != HaState::Connected) return ErrorCode::AdapterUnavailable;
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    if (requested_ || mailboxStatus_ == DiscoveryStatus::Running) return ErrorCode::Busy;
    requested_ = true;
    generation_++;
    mailboxStatus_ = DiscoveryStatus::Running;
    mailboxResult_ = HaDiscovery();
    mailboxError_ = ErrorCode::Ok;
    return ErrorCode::Ok;
}

void HomeAssistantClient::cancelDiscovery() {
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    if (mailboxStatus_ != DiscoveryStatus::Running) return;
    generation_++;  // its result, if it still comes, is dropped
    if (requested_) {
        requested_ = false;  // never started
    } else {
        cancelRequested_ = true;  // the integration task stops it
    }
    mailboxStatus_ = DiscoveryStatus::Idle;
    mailboxResult_ = HaDiscovery();
}

DiscoveryStatus HomeAssistantClient::takeDiscovery(HaDiscovery *out, ErrorCode *error) {
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    const DiscoveryStatus status = mailboxStatus_;
    if (status == DiscoveryStatus::Done && out != nullptr) *out = std::move(mailboxResult_);
    if (status == DiscoveryStatus::Failed && error != nullptr) *error = mailboxError_;
    if (status == DiscoveryStatus::Done || status == DiscoveryStatus::Failed) {
        mailboxStatus_ = DiscoveryStatus::Idle;
        mailboxResult_ = HaDiscovery();
    }
    return status;
}

}  // namespace hubbo::integrations
