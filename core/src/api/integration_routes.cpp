// GET/PUT /admin/integrations/{id} (settings; the HA token and the MQTT
// password are write-only)
// and POST/GET/DELETE /admin/integrations/{id}/discover (HA candidates), and
// POST/GET .../entities/{entity_id}/details (what registering an entity stores).
//
// The HA access token (kHaTokenKey) and the MQTT broker password
// (kMqttPasswordKey) live in the SecretStore. They're read here only to hand
// them to the client or bridge, and never put in a response.

#include <algorithm>
#include <cctype>
#include <optional>

#include "hubbo/auth/secret_store.h"
#include "hubbo/config/json.h"
#include "hubbo/config/service.h"
#include "hubbo/domain/ids.h"
#include "hubbo/domain/limits.h"
#include "hubbo/ha/capabilities.h"
#include "hubbo/integrations/ha_client.h"
#include "hubbo/integrations/mqtt_bridge.h"
#include "routes.h"
#include "support.h"

namespace hubbo::api {

const DeviceSpec *registeredDevice(const ApiContext &ctx, std::string_view entityId, std::string_view uniqueId,
                                   std::string_view platform) {
    const DeviceSpec *byUniqueId = nullptr;
    for (const DeviceSpec &d : ctx.config.config().devices) {
        if (d.integrationId != kHaIntegrationId || d.provider.type != kHomeAssistantType) continue;
        if (d.provider.entityId == entityId) return &d;
        if (byUniqueId == nullptr && !uniqueId.empty() && d.provider.uniqueId == uniqueId && d.haCapabilities &&
            d.haCapabilities->platform == platform) {
            byUniqueId = &d;
        }
    }
    return byUniqueId;
}

const char *fetchStateName(FetchState state) {
    switch (state) {
        case FetchState::Running: return "running";
        case FetchState::Done: return "done";
        case FetchState::Failed: return "failed";
        case FetchState::Idle: break;
    }
    return "idle";
}

namespace {

// "http(s)://<host>[/]" -> <host> and the matching WebSocket scheme, or
// nullptr if it isn't an http(s) URL with a host.
const char *splitBaseUrl(std::string_view url, std::string_view *host) {
    const char *scheme = nullptr;
    if (url.rfind("http://", 0) == 0) {
        scheme = "ws://";
        url.remove_prefix(7);
    } else if (url.rfind("https://", 0) == 0) {
        scheme = "wss://";
        url.remove_prefix(8);
    } else {
        return nullptr;
    }
    if (!url.empty() && url.back() == '/') url.remove_suffix(1);
    if (url.empty()) return nullptr;
    *host = url;
    return scheme;
}

}  // namespace

std::string homeAssistantSocketUrl(std::string_view baseUrl) {
    std::string_view host;
    const char *scheme = splitBaseUrl(baseUrl, &host);
    return scheme == nullptr ? std::string() : scheme + std::string(host) + "/api/websocket";
}

auth::SecretRead applyHomeAssistantSettings(const config::ConfigService &config, const auth::SecretStore *secrets,
                                            integrations::HomeAssistantClient &client) {
    const IntegrationSpec *integration = config.findIntegration(kHaIntegrationId);
    std::string url;
    if (integration != nullptr && integration->enabled) url = homeAssistantSocketUrl(integration->url);
    std::string token;
    const auth::SecretRead read =
        secrets != nullptr ? secrets->read(kHaTokenKey, &token) : auth::SecretRead::NotFound;
    if (read != auth::SecretRead::Ok) token.clear();
    client.configure(std::move(url), std::move(token), integration != nullptr ? integration->verifyTls : true);
    return read;
}

auth::SecretRead applyMqttSettings(const config::ConfigService &config, const auth::SecretStore *secrets,
                                   integrations::MqttBridge &bridge) {
    const IntegrationSpec *integration = config.findIntegration(kMqttIntegrationId);
    integrations::MqttSettings settings;
    if (integration != nullptr) {
        settings.enabled = integration->enabled;
        settings.host = integration->host;
        settings.port = integration->port;
        settings.username = integration->username;
    }
    const auth::SecretRead read =
        secrets != nullptr ? secrets->read(kMqttPasswordKey, &settings.password) : auth::SecretRead::NotFound;
    if (read != auth::SecretRead::Ok) settings.password.clear();
    bridge.configure(std::move(settings));
    return read;
}

namespace {

// ^https?://[^/@?#\s]+(:[0-9]{1,5})?/?$ (HomeAssistantSettings.url): a base
// URL with no credentials, path, query or fragment. ("" unsets it; the
// caller allows that.)
bool isValidBaseUrl(std::string_view url) {
    std::string_view host;
    if (url.size() > limits::kMaxUrlLength || splitBaseUrl(url, &host) == nullptr) return false;
    for (char c : host) {
        if (c == '/' || c == '@' || c == '?' || c == '#' || std::isspace(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

const IntegrationSpec *requireIntegration(const ApiContext &ctx, std::string_view id, http::Response &out) {
    const IntegrationSpec *integration = ctx.config.findIntegration(id);
    if (integration == nullptr) sendError(out, 404, "not_found", "No such integration");
    return integration;
}

// The integration type's write-only secret: its SecretStore key, the
// settings field that sets it, the flag that reports it, what a restore
// asks the admin for, and the setting it was entered for (a new endpoint
// never gets the old secret).
struct SecretSlot {
    const char *integrationId;
    const char *type;
    const char *key;
    const char *field;
    const char *presentField;
    const char *followUp;
    std::string IntegrationSpec::*endpoint;
};
constexpr SecretSlot kSecretSlots[] = {
    {kHaIntegrationId, kHomeAssistantType, kHaTokenKey, "access_token", "has_access_token", "home_assistant_token",
     &IntegrationSpec::url},
    {kMqttIntegrationId, kMqttType, kMqttPasswordKey, "password", "has_password", "mqtt_password",
     &IntegrationSpec::host},
};

const SecretSlot *secretSlot(std::string_view type) {
    for (const SecretSlot &slot : kSecretSlots) {
        if (type == slot.type) return &slot;
    }
    return nullptr;
}

// The secret's presence; false (after a 503) if the store can't say.
bool hasSecret(const ApiContext &ctx, const char *key, http::Response &out, bool *present) {
    *present = false;
    if (ctx.secrets == nullptr) return true;
    std::string ignored;
    switch (ctx.secrets->read(key, &ignored)) {
        case auth::SecretRead::Ok: *present = true; return true;
        case auth::SecretRead::NotFound: return true;
        default:
            sendError(out, 503, "storage_unavailable", "The secret store can't be read");
            return false;
    }
}

void sendConfig(const ApiContext &ctx, const IntegrationSpec &integration, http::Response &out, int status = 200) {
    JsonDocument doc;
    JsonObject body = doc.to<JsonObject>();
    config::encodeIntegration(integration, body);
    JsonObject settings = body["settings"];
    if (const SecretSlot *slot = secretSlot(integration.type)) {
        bool present = false;
        if (!hasSecret(ctx, slot->key, out, &present)) return;
        settings[slot->presentField] = present;
    }
    sendJson(out, status, doc);
}

// A field that may be a string or null.
struct Nullable {
    bool present = false;
    std::optional<std::string> value;  // nullopt: null
};

bool readNullable(JsonVariantConst v, size_t minLength, size_t maxLength, Nullable *out, ValidationReport *report,
                  const std::string &path) {
    if (v.isUnbound()) return true;
    out->present = true;
    if (v.isNull()) return true;
    if (!v.is<const char *>()) {
        report->add(ErrorCode::WrongType, path);
        return false;
    }
    std::string text = v.as<const char *>();
    if (text.size() > maxLength) {
        report->add(ErrorCode::TooLong, path);
        return false;
    }
    if (text.size() < minLength) {
        report->add(ErrorCode::Required, path);
        return false;
    }
    out->value = std::move(text);
    return true;
}

struct SettingsUpdate {
    std::optional<std::string> url;
    std::optional<bool> verifyTls;
    Nullable secret;  // access_token / password
    std::optional<std::string> deviceName;
    std::optional<std::string> host;
    std::optional<uint16_t> port;
    std::optional<std::string> username;
};

// A string `valid` accepts (TooLong past `max` bytes, else WrongType).
bool readText(JsonVariantConst v, size_t max, bool (*valid)(std::string_view), std::optional<std::string> *out,
              ValidationReport *report, const std::string &path) {
    if (!v.is<const char *>()) {
        report->add(ErrorCode::WrongType, path);
        return false;
    }
    const std::string_view text = v.as<const char *>();
    if (text.size() > max || !valid(text)) {
        report->add(text.size() > max ? ErrorCode::TooLong : ErrorCode::WrongType, path);
        return false;
    }
    *out = std::string(text);
    return true;
}

// HomeAssistantSettingsUpdate / BleHidSettingsUpdate / MqttSettingsUpdate
// for `type`.
bool decodeSettings(JsonVariantConst v, const std::string &type, SettingsUpdate *out, ValidationReport *report) {
    const JsonObjectConst settings = v.as<JsonObjectConst>();
    const char *given = settings["type"] | "";
    if (settings["type"].isUnbound()) {
        report->add(ErrorCode::Required, "/settings/type");
        return false;
    }
    if (type != given || (type != kHomeAssistantType && type != kBleHidType && type != kMqttType)) {
        report->add(ErrorCode::IntegrationMismatch, "/settings/type");
        return false;
    }
    const SecretSlot *slot = secretSlot(type);
    bool ok = true;
    for (JsonPairConst pair : settings) {
        const std::string key = pair.key().c_str();
        const std::string path = "/settings/" + key;
        const JsonVariantConst value = pair.value();
        if (key == "type") continue;
        if (type == kHomeAssistantType && key == "url") {
            if (!value.is<const char *>()) {
                report->add(ErrorCode::WrongType, path);
                ok = false;
            } else if (*value.as<const char *>() != '\0' && !isValidBaseUrl(value.as<const char *>())) {
                report->add(std::string_view(value.as<const char *>()).size() > limits::kMaxUrlLength
                                ? ErrorCode::TooLong
                                : ErrorCode::WrongType,
                            path);
                ok = false;
            } else {
                out->url = value.as<const char *>();
            }
        } else if (type == kHomeAssistantType && key == "verify_tls") {
            if (!value.is<bool>()) {
                report->add(ErrorCode::WrongType, path);
                ok = false;
            } else {
                out->verifyTls = value.as<bool>();
            }
        } else if (slot != nullptr && key == slot->field) {
            ok = readNullable(value, 1, auth::kMaxSecretValueLength, &out->secret, report, path) && ok;
        } else if (type == kMqttType && key == "host") {
            ok = readText(value, limits::kMaxHostLength, isValidMqttHost, &out->host, report, path) && ok;
        } else if (type == kMqttType && key == "username") {
            ok = readText(value, limits::kMaxUsernameLength, isValidMqttUsername, &out->username, report, path) && ok;
        } else if (type == kMqttType && key == "port") {
            if (!value.is<int64_t>()) {
                report->add(ErrorCode::WrongType, path);
                ok = false;
            } else if (value.as<int64_t>() < 1 || value.as<int64_t>() > 65535) {
                report->add(ErrorCode::ParamOutOfRange, path);
                ok = false;
            } else {
                out->port = static_cast<uint16_t>(value.as<int64_t>());
            }
        } else if (type == kBleHidType && key == "device_name") {
            if (!value.is<const char *>()) {
                report->add(ErrorCode::WrongType, path);
                ok = false;
            } else {
                const std::string_view name = value.as<const char *>();
                if (name.empty() || name.size() > limits::kMaxBleNameLength) {
                    report->add(name.empty() ? ErrorCode::Required : ErrorCode::TooLong, path);
                    ok = false;
                } else {
                    out->deviceName = std::string(name);
                }
            }
        } else {
            report->add(ErrorCode::UnknownField, path);
            ok = false;
        }
    }
    return ok;
}

// ---- discovery ------------------------------------------------------------

const char *discoveryError(ErrorCode code) {
    switch (code) {
        case ErrorCode::AdapterUnavailable: return "Home Assistant disconnected during discovery";
        case ErrorCode::AdapterTimeout: return "Home Assistant didn't answer in time";
        case ErrorCode::AdapterFailed: return "Home Assistant refused a discovery request";
        case ErrorCode::MalformedJson: return "Home Assistant sent a reply Hubbo can't read";
        default: return errorCodeName(code);
    }
}

// Normalizes a finished discovery: the commands each candidate would offer
// from its domain alone (discovery has no services; the details add them).
void fillCache(DiscoveryCache *cache, integrations::HaDiscovery discovery) {
    const ha::ServiceDirectory noServices;
    const std::function<void()> yield = cache->yield;
    cache->reset(DiscoveryCache::State::Done);
    cache->candidates.reserve(discovery.candidates.size());
    // About 0.5 ms a candidate on the board: let other tasks run every 32.
    size_t done = 0;
    for (auto &c : discovery.candidates) {
        if (yield && ++done % 32 == 0) yield();
        DiscoveryCache::Candidate out;
        ha::ServiceDirectory::Offer offer;
        const ha::CapabilitySnapshot snapshot = ha::makeSnapshot(c.info, c.platform, noServices, &offer);
        const ha::EntityCapabilities caps = ha::EntityCapabilities::fromSnapshot(c.entityId, snapshot);
        const CommandCatalog catalog = caps.catalog();
        for (uint16_t i = 0; i < catalog.count; i++) out.commandIds.emplace_back(catalog.items[i].id);
        out.droppedOptions = offer.droppedOptions + caps.droppedOptions();
        out.suggestedKind = ha::suggestedKind(c.domain, c.platform, c.deviceClass);
        out.entityId = std::move(c.entityId);
        out.name = std::move(c.name);
        out.domain = std::move(c.domain);
        out.platform = std::move(c.platform);
        out.state = std::move(c.state);
        cache->candidates.push_back(std::move(out));
    }
    cache->truncated = discovery.truncated;
    cache->elapsedMs = discovery.elapsedMs;
    cache->generation = discovery.generation;
}

// Picks up what the client's mailbox has for us.
void syncDiscovery(ApiContext &ctx) {
    DiscoveryCache &cache = *ctx.discovery;
    integrations::HaDiscovery result;
    ErrorCode error = ErrorCode::Ok;
    switch (ctx.homeAssistant->takeDiscovery(&result, &error)) {
        case integrations::DiscoveryStatus::Done: fillCache(&cache, std::move(result)); break;
        case integrations::DiscoveryStatus::Failed:
            cache.reset(DiscoveryCache::State::Failed);
            cache.error = discoveryError(error);
            break;
        case integrations::DiscoveryStatus::Running: cache.state = DiscoveryCache::State::Running; break;
        case integrations::DiscoveryStatus::Idle:
            // Cancelled, or dropped with the connection: nothing will come.
            if (cache.state == DiscoveryCache::State::Running) cache.state = DiscoveryCache::State::Idle;
            break;
    }
    // Candidates of other settings, a reconnect or an older discovery.
    if (cache.state == DiscoveryCache::State::Done &&
        cache.generation != ctx.homeAssistant->discoveryGeneration()) {
        cache.reset(DiscoveryCache::State::Idle);
    }
}


// The HA integration with a client; false after sending 404 / 503.
bool requireDiscovery(const ApiContext &ctx, std::string_view id, http::Response &out) {
    const IntegrationSpec *integration = requireIntegration(ctx, id, out);
    if (integration == nullptr) return false;
    if (integration->type != kHomeAssistantType) {
        sendError(out, 404, "not_found", "Only Home Assistant has discovery");
        return false;
    }
    if (ctx.homeAssistant == nullptr) {
        sendError(out, 503, "integration_unavailable", "This hub has no Home Assistant client");
        return false;
    }
    return true;
}

bool containsFolded(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    auto fold = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                [&](char a, char b) { return fold(a) == fold(b); });
    return it != haystack.end();
}

struct Page {
    std::string q;
    std::string domain;
    bool available = false;  // only supported entities no device is registered for
    size_t limit = 50;
    size_t offset = 0;
};

bool readPage(const http::Request &request, http::Response &out, Page *page) {
    page->q = http::queryValue(request.query, "q");
    if (page->q.size() > 48) {
        sendDetail(out, "/q", "too_long", "q is at most 48 characters");
        return false;
    }
    page->domain = http::queryValue(request.query, "domain");
    // The allowlisted domains: ha::kEntityDomains is their one list.
    if (!page->domain.empty() && ha::domainNamed(page->domain) == ha::EntityDomain::Unsupported) {
        sendDetail(out, "/domain", "invalid_enum_value", "domain must be " + ha::entityDomainList());
        return false;
    }
    const std::string available = http::queryValue(request.query, "available");
    if (!available.empty() && available != "true" && available != "false") {
        sendDetail(out, "/available", "wrong_type", "available must be true or false");
        return false;
    }
    page->available = available == "true";
    auto number = [](std::string_view text, size_t max, size_t *value) {
        uint64_t n = 0;
        if (!parseDecimal(text, max, &n)) return false;
        *value = static_cast<size_t>(n);
        return true;
    };
    const std::string limit = http::queryValue(request.query, "limit");
    if (!limit.empty() && (!number(limit, 100, &page->limit) || page->limit == 0)) {
        sendDetail(out, "/limit", "param_out_of_range", "limit must be 1-100");
        return false;
    }
    const std::string cursor = http::queryValue(request.query, "cursor");
    if (!cursor.empty() && !number(cursor, integrations::HomeAssistantClient::kMaxCandidates, &page->offset)) {
        sendDetail(out, "/cursor", "pattern", "cursor must be a next_cursor from an earlier page");
        return false;
    }
    return true;
}

// The fields a discovery candidate and entity details share.
void putEntity(JsonObject out, const ApiContext &ctx, const HaEntity &e) {
    JsonObject provider = out["provider"].to<JsonObject>();
    provider["type"] = kHomeAssistantType;
    provider["entity_id"] = e.entityId;
    if (!e.uniqueId.empty()) provider["unique_id"] = e.uniqueId;
    out["name"] = e.name.empty() ? e.entityId : e.name;
    out["domain"] = e.domain;
    if (!e.platform.empty()) out["platform"] = e.platform;
    out["supported"] = !e.commandIds.empty();
    JsonArray ids = out["command_ids"].to<JsonArray>();
    for (const std::string &id : e.commandIds) ids.add(id);
    out["dropped_options"] = e.droppedOptions;
    const DeviceSpec *device = registeredDevice(ctx, e.entityId, e.uniqueId, e.platform);
    putNullableId(out, "registered_device_id", device != nullptr ? std::string_view(device->id) : std::string_view());
}

void putCandidate(JsonObject out, const ApiContext &ctx, const DiscoveryCache::Candidate &c) {
    putEntity(out, ctx, c);
    if (!c.state.empty()) out["state"] = c.state;
    out["suggested_kind"] = c.suggestedKind;
}

// ---- entity details ---------------------------------------------------------

std::string detailsError(ErrorCode code, const std::string &entityId) {
    switch (code) {
        case ErrorCode::EntityNotFound: return entityId + " isn't in Home Assistant";
        case ErrorCode::AdapterUnavailable: return "Home Assistant disconnected";
        default: return discoveryError(code);
    }
}

// Normalizes fetched details: the snapshot registration stores.
void fillDetails(EntityDetails *out, integrations::HaEntityDetails in) {
    ha::ServiceDirectory::Offer offer;
    {
        const ha::ServiceDirectory services =
            ha::ServiceDirectory::parse(in.services, ha::serviceDomains(in.entityId, in.platform), in.translations);
        std::string().swap(in.services);  // about 230 KB
        std::string().swap(in.translations);
        out->snapshot = ha::makeSnapshot(in.info, in.platform, services, &offer);
    }
    const ha::EntityCapabilities caps = ha::EntityCapabilities::fromSnapshot(in.entityId, out->snapshot);
    const CommandCatalog catalog = caps.catalog();
    for (uint16_t i = 0; i < catalog.count; i++) out->commandIds.emplace_back(catalog.items[i].id);
    out->skipped = std::move(offer.skipped);
    out->droppedOptions = offer.droppedOptions + caps.droppedOptions();
    out->uniqueId = std::move(in.uniqueId);
    out->name = std::move(in.name);
    out->domain = std::string(in.entityId.substr(0, in.entityId.find('.')));
    out->platform = out->snapshot.platform;  // as normalized and stored ("" if invalid)
}

// Drops the client's details fetch and the cached result.
void dropDetails(ApiContext &ctx) {
    ctx.homeAssistant->cancelEntityDetails();
    ctx.homeAssistant->takeEntityDetails(nullptr, nullptr);
    ctx.discovery->details.reset(FetchState::Idle);
}

// Picks up the client's details result for the entity being fetched.
void syncDetails(ApiContext &ctx) {
    EntityDetails &details = ctx.discovery->details;
    integrations::HaEntityDetails result;
    ErrorCode error = ErrorCode::Ok;
    const integrations::DiscoveryStatus status = ctx.homeAssistant->takeEntityDetails(&result, &error);
    if (details.state != FetchState::Running || status == integrations::DiscoveryStatus::Running) return;
    if (status == integrations::DiscoveryStatus::Done) {
        if (result.entityId != details.entityId) return;
        details.reset(FetchState::Done, details.entityId);
        return fillDetails(&details, std::move(result));
    }
    // Failed, or Idle: dropped with the connection or new settings, nothing will come.
    details.state = FetchState::Failed;
    details.error =
        detailsError(status == integrations::DiscoveryStatus::Failed ? error : ErrorCode::AdapterUnavailable,
                     details.entityId);
}

// EntityDetailsStatus for `entityId` (idle if the details are another entity's).
void sendDetails(const ApiContext &ctx, std::string_view entityId, http::Response &out, int status) {
    const EntityDetails &d = ctx.discovery->details;
    JsonDocument doc;
    if (d.state == FetchState::Idle || d.entityId != entityId) {
        doc["state"] = "idle";
        return sendJson(out, status, doc);
    }
    doc["state"] = fetchStateName(d.state);
    doc["entity_id"] = d.entityId;
    if (d.state == FetchState::Failed) doc["error"] = d.error;
    if (d.state == FetchState::Done) {
        JsonObject body = doc.as<JsonObject>();
        putEntity(body, ctx, d);
        JsonArray skipped = body["skipped_services"].to<JsonArray>();
        for (const auto &s : d.skipped) {
            JsonObject o = skipped.add<JsonObject>();
            o["service"] = s.service;
            o["reason"] = ha::ServiceDirectory::skipReasonName(s.reason);
        }
    }
    sendJson(out, status, doc);
}

void sendDiscovery(const ApiContext &ctx, const Page &page, http::Response &out, int status) {
    const DiscoveryCache &cache = *ctx.discovery;
    JsonDocument doc;
    doc["state"] = fetchStateName(cache.state);
    if (cache.state == DiscoveryCache::State::Failed) doc["error"] = cache.error;
    if (cache.state == DiscoveryCache::State::Done) {
        doc["total"] = cache.candidates.size();
        doc["truncated"] = cache.truncated;
        doc["elapsed_ms"] = cache.elapsedMs;
        JsonArray list = doc["candidates"].to<JsonArray>();
        size_t matched = 0;
        bool more = false;
        for (const DiscoveryCache::Candidate &c : cache.candidates) {
            if (!page.domain.empty() && c.domain != page.domain) continue;
            if (!containsFolded(c.entityId, page.q) && !containsFolded(c.name, page.q)) continue;
            if (page.available &&
                (c.commandIds.empty() || registeredDevice(ctx, c.entityId, c.uniqueId, c.platform) != nullptr)) {
                continue;
            }
            if (matched++ < page.offset) continue;
            if (list.size() == page.limit) {
                more = true;
                break;
            }
            putCandidate(list.add<JsonObject>(), ctx, c);
        }
        if (more) {
            doc["next_cursor"] = std::to_string(page.offset + page.limit);
        } else {
            doc["next_cursor"] = nullptr;
        }
    }
    sendJson(out, status, doc);
}

}  // namespace

bool forgetSecretsForNewEndpoints(ApiContext &ctx, const ConfigSpec &next, http::Response &out) {
    if (ctx.secrets == nullptr) return true;
    for (const SecretSlot &slot : kSecretSlots) {
        const IntegrationSpec *current = ctx.config.findIntegration(slot.integrationId);
        const int index = indexOfId(next.integrations, slot.integrationId);
        const std::string_view nextEndpoint =
            index < 0 ? std::string_view() : next.integrations[static_cast<size_t>(index)].*slot.endpoint;
        if (current == nullptr || current->*slot.endpoint == nextEndpoint) continue;
        if (!ctx.secrets->erase(slot.key)) {
            // An earlier slot's secret may be gone already: the clients follow the store.
            reapplyIntegrations(ctx, true);
            sendError(out, 503, "storage_unavailable", "A stored integration secret couldn't be erased");
            return false;
        }
    }
    return true;
}

void putSecretFollowUps(JsonArray out, const ConfigSpec &restored) {
    for (const SecretSlot &slot : kSecretSlots) {
        const int index = indexOfId(restored.integrations, slot.integrationId);
        if (index >= 0 && !(restored.integrations[static_cast<size_t>(index)].*slot.endpoint).empty()) {
            out.add(slot.followUp);
        }
    }
}

const EntityDetails *completedDetails(ApiContext &ctx, std::string_view entityId) {
    if (ctx.homeAssistant == nullptr) return nullptr;
    syncDetails(ctx);
    const EntityDetails &d = ctx.discovery->details;
    return d.state == FetchState::Done && d.entityId == entityId ? &d : nullptr;
}

void reapplyIntegrations(ApiContext &ctx, bool haChanged) {
    if (ctx.homeAssistant != nullptr) {
        if (haChanged) {
            ctx.homeAssistant->cancelDiscovery();
            ctx.homeAssistant->takeDiscovery(nullptr, nullptr);  // an unread result of the old settings
            ctx.discovery->reset(DiscoveryCache::State::Idle);
            dropDetails(ctx);
        }
        applyHomeAssistantSettings(ctx.config, ctx.secrets, *ctx.homeAssistant);
    }
    if (ctx.mqtt != nullptr) applyMqttSettings(ctx.config, ctx.secrets, *ctx.mqtt);
}

void addIntegrationRoutes(RouteSink &sink, ApiContext &ctx) {
    sink.add("/api/v1/admin/integrations/{integration_id}", auth::Operation::GetIntegrationConfig,
             [&ctx](auto &, auto &p, auto &out) {
                 const IntegrationSpec *integration = requireIntegration(ctx, p[0], out);
                 if (integration != nullptr) sendConfig(ctx, *integration, out);
             });

    sink.add("/api/v1/admin/integrations/{integration_id}", auth::Operation::ConfigureIntegration,
             [&ctx](auto &request, auto &p, auto &out) {
                 const IntegrationSpec *current = requireIntegration(ctx, p[0], out);
                 if (current == nullptr) return;
                 JsonDocument doc;
                 if (!readJson(request, out, doc)) return;
                 ValidationReport report;
                 SettingsUpdate settings;
                 if (checkObject(doc.as<JsonVariantConst>(),
                                 {{"name", Field::Kind::String, false, limits::kMaxNameLength},
                                  {"enabled", Field::Kind::Bool, false},
                                  {"settings", Field::Kind::Object, false},
                                  {"revision", Field::Kind::Integer, true}},
                                 &report) &&
                     !doc["settings"].isNull()) {
                     decodeSettings(doc["settings"], current->type, &settings, &report);
                 }
                 if (!report.ok()) return sendIssues(out, report, "The integration settings are invalid");
                 const int64_t revision = doc["revision"].as<int64_t>();
                 if (revision < 1 || revision > UINT32_MAX) {
                     return sendDetail(out, "/revision", "param_out_of_range", "revision must be a positive integer");
                 }

                 IntegrationSpec next = *current;
                 if (doc["name"].is<const char *>()) next.name = doc["name"].as<const char *>();
                 if (doc["enabled"].is<bool>()) next.enabled = doc["enabled"].as<bool>();
                 if (settings.url) next.url = *settings.url;
                 if (settings.verifyTls) next.verifyTls = *settings.verifyTls;
                 if (settings.deviceName) next.deviceName = *settings.deviceName;
                 if (settings.host) next.host = *settings.host;
                 if (settings.port) next.port = *settings.port;
                 if (settings.username) next.username = *settings.username;

                 // A new URL or host never gets the old secret: it's erased
                 // before the change is saved (and stays erased if the save
                 // then fails).
                 const SecretSlot *slot = secretSlot(current->type);
                 const bool setSecret = settings.secret.present && settings.secret.value.has_value();
                 const bool clearSecret =
                     slot != nullptr && ((settings.secret.present && !setSecret) ||
                                         next.*slot->endpoint != current->*slot->endpoint);
                 const bool haChanged = current->type == kHomeAssistantType &&
                                        (setSecret || clearSecret || next.enabled != current->enabled ||
                                         next.verifyTls != current->verifyTls);
                 if ((setSecret || clearSecret) && ctx.secrets == nullptr) {
                     return sendError(out, 503, "storage_unavailable", "This hub has no secret store");
                 }
                 if (clearSecret && !ctx.secrets->erase(slot->key)) {
                     return sendError(out, 503, "storage_unavailable", "The stored secret couldn't be erased");
                 }
                 const std::string id = current->id;  // `current` dies with the commit
                 const config::Outcome outcome =
                     ctx.config.replaceIntegration(id, std::move(next), static_cast<uint32_t>(revision));
                 if (!outcome.ok()) {
                     if (clearSecret) reapplyIntegrations(ctx, haChanged);
                     return sendOutcome(out, outcome, "integration");
                 }
                 const bool secretSaved = !setSecret || ctx.secrets->write(slot->key, *settings.secret.value);
                 reapplyIntegrations(ctx, haChanged);
                 if (!secretSaved) {
                     return sendError(out, 503, "storage_unavailable",
                                      "The settings were saved but the secret couldn't be stored; send it again");
                 }
                 sendConfig(ctx, *ctx.config.findIntegration(id), out);
             });

    sink.add("/api/v1/admin/integrations/{integration_id}/discover", auth::Operation::DiscoverIntegration,
             [&ctx](auto &, auto &p, auto &out) {
                 if (!requireDiscovery(ctx, p[0], out)) return;
                 syncDiscovery(ctx);
                 switch (ctx.homeAssistant->requestDiscovery()) {
                     case ErrorCode::Ok:
                     case ErrorCode::Busy: break;  // that one is reported
                     default:
                         return sendError(out, 503, "integration_unavailable", "Home Assistant isn't connected");
                 }
                 ctx.discovery->reset(DiscoveryCache::State::Running);
                 sendDiscovery(ctx, Page{}, out, 202);
             });

    sink.add("/api/v1/admin/integrations/{integration_id}/discover", auth::Operation::GetDiscovery,
             [&ctx](auto &request, auto &p, auto &out) {
                 if (!requireDiscovery(ctx, p[0], out)) return;
                 Page page;
                 if (!readPage(request, out, &page)) return;
                 syncDiscovery(ctx);
                 sendDiscovery(ctx, page, out, 200);
             });

    sink.add("/api/v1/admin/integrations/{integration_id}/entities/{entity_id}/details",
             auth::Operation::RequestEntityDetails, [&ctx](auto &, auto &p, auto &out) {
                 if (!requireDiscovery(ctx, p[0], out)) return;
                 const std::string entityId(p[1]);
                 if (!ha::isValidEntityId(entityId) || ha::entityDomain(entityId) == ha::EntityDomain::Unsupported) {
                     return sendDetail(out, "/entity_id", "pattern",
                                       "entity_id must be one " + ha::entityDomainList() + " entity");
                 }
                 // One at a time: a new pick replaces the earlier fetch (the
                 // cancel frees the slot, and the id is checked above).
                 dropDetails(ctx);
                 if (ctx.homeAssistant->requestEntityDetails(entityId) != ErrorCode::Ok) {
                     return sendError(out, 503, "integration_unavailable", "Home Assistant isn't connected");
                 }
                 ctx.discovery->details.reset(FetchState::Running, entityId);
                 sendDetails(ctx, entityId, out, 202);
             });

    sink.add("/api/v1/admin/integrations/{integration_id}/entities/{entity_id}/details",
             auth::Operation::GetEntityDetails, [&ctx](auto &, auto &p, auto &out) {
                 if (!requireDiscovery(ctx, p[0], out)) return;
                 syncDetails(ctx);
                 sendDetails(ctx, p[1], out, 200);
             });

    sink.add("/api/v1/admin/integrations/{integration_id}/discover", auth::Operation::CancelDiscovery,
             [&ctx](auto &, auto &p, auto &out) {
                 if (!requireDiscovery(ctx, p[0], out)) return;
                 ctx.homeAssistant->cancelDiscovery();
                 syncDiscovery(ctx);  // a result that beat the cancel
                 ctx.discovery->reset(DiscoveryCache::State::Idle);
                 sendEmpty(out, 204);
             });
}

}  // namespace hubbo::api
