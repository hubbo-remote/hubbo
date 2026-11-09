// GET/PUT /admin/integrations/{id} (settings; the HA token is write-only)
// and POST/GET/DELETE /admin/integrations/{id}/discover (HA candidates).
//
// The HA access token lives in the SecretStore under kHaTokenKey. It is
// read here only to hand it to the client, and never put in a response.

#include <algorithm>
#include <cctype>
#include <optional>

#include "hubbo/auth/secret_store.h"
#include "hubbo/config/json.h"
#include "hubbo/config/service.h"
#include "hubbo/domain/limits.h"
#include "hubbo/ha/capabilities.h"
#include "hubbo/integrations/ha_client.h"
#include "routes.h"
#include "support.h"

namespace hubbo::api {

const DiscoveryCache::Candidate *DiscoveryCache::find(std::string_view entityId, uint32_t currentGeneration) const {
    if (state != State::Done || generation != currentGeneration) return nullptr;
    for (const Candidate &c : candidates) {
        if (c.entityId == entityId) return &c;
    }
    return nullptr;
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

// The token's presence; false (after a 503) if the store can't say.
bool hasToken(const ApiContext &ctx, http::Response &out, bool *present) {
    *present = false;
    if (ctx.secrets == nullptr) return true;
    std::string ignored;
    switch (ctx.secrets->read(kHaTokenKey, &ignored)) {
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
    if (integration.type == kHomeAssistantType) {
        bool present = false;
        if (!hasToken(ctx, out, &present)) return;
        settings["has_access_token"] = present;
    } else if (integration.type == kBleHidType) {
        const std::string state = ctx.runtime.integrationState(integration.id);
        settings["bonded"] = state != "not_configured" && state != "disabled";
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
    Nullable accessToken;
    std::optional<std::string> deviceName;
};

// HomeAssistantSettingsUpdate / BleHidSettingsUpdate for `type`.
bool decodeSettings(JsonVariantConst v, const std::string &type, SettingsUpdate *out, ValidationReport *report) {
    const JsonObjectConst settings = v.as<JsonObjectConst>();
    const char *given = settings["type"] | "";
    if (settings["type"].isUnbound()) {
        report->add(ErrorCode::Required, "/settings/type");
        return false;
    }
    if (type != given || (type != kHomeAssistantType && type != kBleHidType)) {
        report->add(ErrorCode::IntegrationMismatch, "/settings/type");
        return false;
    }
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
        } else if (type == kHomeAssistantType && key == "access_token") {
            ok = readNullable(value, 1, auth::kMaxSecretValueLength, &out->accessToken, report, path) && ok;
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

// Normalizes a finished discovery: the snapshot each candidate would be
// registered with, and the commands it would offer.
void fillCache(DiscoveryCache *cache, integrations::HaDiscovery discovery) {
    std::vector<std::string> platforms;
    for (const auto &c : discovery.candidates) {
        if (c.platform.empty() || c.platform == c.domain) continue;
        if (std::find(platforms.begin(), platforms.end(), c.platform) == platforms.end()) {
            platforms.push_back(c.platform);
        }
    }
    const ha::ServiceDirectory services = ha::ServiceDirectory::parse(discovery.services, platforms);
    std::string().swap(discovery.services);  // can be a few hundred KB
    const std::function<void()> yield = cache->yield;
    if (yield) yield();

    cache->reset(DiscoveryCache::State::Done);
    cache->candidates.reserve(discovery.candidates.size());
    // About 0.5 ms a candidate on the board: let other tasks run every 32.
    size_t done = 0;
    for (auto &c : discovery.candidates) {
        if (yield && ++done % 32 == 0) yield();
        DiscoveryCache::Candidate out;
        ha::ServiceDirectory::Offer offer;
        out.snapshot = ha::makeSnapshot(c.info, c.platform, services, &offer);
        const ha::EntityCapabilities caps = ha::EntityCapabilities::fromSnapshot(c.entityId, out.snapshot);
        const CommandCatalog catalog = caps.catalog();
        for (uint16_t i = 0; i < catalog.count; i++) out.commandIds.emplace_back(catalog.items[i].id);
        out.skipped = std::move(offer.skipped);  // offerFor caps it at kMaxSnapshotServices
        out.droppedOptions = offer.droppedOptions + caps.droppedOptions();
        out.entityId = std::move(c.entityId);
        out.uniqueId = std::move(c.uniqueId);
        out.name = std::move(c.name);
        out.domain = std::move(c.domain);
        out.platform = std::move(c.platform);
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

const char *cacheStateName(DiscoveryCache::State state) {
    switch (state) {
        case DiscoveryCache::State::Idle: return "idle";
        case DiscoveryCache::State::Running: return "running";
        case DiscoveryCache::State::Done: return "done";
        case DiscoveryCache::State::Failed: return "failed";
    }
    return "idle";
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
    // The allowlisted domains: ha::entityDomain() is their one list.
    if (!page->domain.empty() && ha::entityDomain(page->domain + ".x") == ha::EntityDomain::Unsupported) {
        sendDetail(out, "/domain", "invalid_enum_value", "domain must be media_player, light or switch");
        return false;
    }
    auto number = [](std::string_view text, size_t max, size_t *value) {
        if (text.empty() || text.size() > 6) return false;
        size_t n = 0;
        for (char c : text) {
            if (c < '0' || c > '9') return false;
            n = n * 10 + static_cast<size_t>(c - '0');
        }
        if (n > max) return false;
        *value = n;
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

const DeviceSpec *registeredDevice(const ApiContext &ctx, const DiscoveryCache::Candidate &c) {
    // entity_id first; else unique_id, which HA keeps unique only per platform.
    const DeviceSpec *byUniqueId = nullptr;
    for (const DeviceSpec &d : ctx.config.config().devices) {
        if (d.integrationId != kHaIntegrationId || d.provider.type != kHomeAssistantType) continue;
        if (d.provider.entityId == c.entityId) return &d;
        if (byUniqueId == nullptr && !c.uniqueId.empty() && d.provider.uniqueId == c.uniqueId &&
            d.haCapabilities && d.haCapabilities->platform == c.platform) {
            byUniqueId = &d;
        }
    }
    return byUniqueId;
}

void putCandidate(JsonObject out, const ApiContext &ctx, const DiscoveryCache::Candidate &c) {
    JsonObject provider = out["provider"].to<JsonObject>();
    provider["type"] = kHomeAssistantType;
    provider["entity_id"] = c.entityId;
    if (!c.uniqueId.empty()) provider["unique_id"] = c.uniqueId;
    out["name"] = c.name.empty() ? c.entityId : c.name;
    out["domain"] = c.domain;
    if (!c.platform.empty()) out["platform"] = c.platform;
    out["supported"] = !c.commandIds.empty();
    JsonArray ids = out["command_ids"].to<JsonArray>();
    for (const std::string &id : c.commandIds) ids.add(id);
    out["dropped_options"] = c.droppedOptions;
    JsonArray skipped = out["skipped_services"].to<JsonArray>();
    for (const std::string &s : c.skipped) skipped.add(s);
    const DeviceSpec *device = registeredDevice(ctx, c);
    putNullableId(out, "registered_device_id", device != nullptr ? std::string_view(device->id) : std::string_view());
}

void sendDiscovery(const ApiContext &ctx, const Page &page, http::Response &out, int status) {
    const DiscoveryCache &cache = *ctx.discovery;
    JsonDocument doc;
    doc["state"] = cacheStateName(cache.state);
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

bool forgetTokenForNewHaUrl(ApiContext &ctx, const ConfigSpec &next, http::Response &out) {
    const IntegrationSpec *current = ctx.config.findIntegration(kHaIntegrationId);
    const int index = indexOfId(next.integrations, kHaIntegrationId);
    const std::string &nextUrl = index < 0 ? std::string() : next.integrations[static_cast<size_t>(index)].url;
    if (current == nullptr || current->url == nextUrl || ctx.secrets == nullptr) return true;
    if (ctx.secrets->erase(kHaTokenKey)) return true;
    sendError(out, 503, "storage_unavailable", "The stored Home Assistant token couldn't be erased");
    return false;
}

void reapplyHomeAssistant(ApiContext &ctx, bool haChanged) {
    if (ctx.homeAssistant == nullptr) return;
    if (haChanged) {
        ctx.homeAssistant->cancelDiscovery();
        ctx.homeAssistant->takeDiscovery(nullptr, nullptr);  // an unread result of the old settings
        ctx.discovery->reset(DiscoveryCache::State::Idle);
    }
    applyHomeAssistantSettings(ctx.config, ctx.secrets, *ctx.homeAssistant);
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

                 // A new URL never gets the old token: it's erased before the
                 // URL is saved (and stays erased if the save then fails).
                 const bool urlChanged = next.url != current->url;
                 const bool setToken = settings.accessToken.present && settings.accessToken.value.has_value();
                 const bool clearToken = (settings.accessToken.present && !setToken) || urlChanged;
                 const bool haChanged = setToken || clearToken || next.enabled != current->enabled ||
                                        next.verifyTls != current->verifyTls;
                 if ((setToken || clearToken) && ctx.secrets == nullptr) {
                     return sendError(out, 503, "storage_unavailable", "This hub has no secret store");
                 }
                 if (clearToken && !ctx.secrets->erase(kHaTokenKey)) {
                     return sendError(out, 503, "storage_unavailable", "The stored token couldn't be erased");
                 }
                 const std::string id = current->id;  // `current` dies with the commit
                 const config::Outcome outcome =
                     ctx.config.replaceIntegration(id, std::move(next), static_cast<uint32_t>(revision));
                 if (!outcome.ok()) {
                     if (clearToken) reapplyHomeAssistant(ctx, true);
                     return sendOutcome(out, outcome, "integration");
                 }
                 const bool tokenSaved = !setToken || ctx.secrets->write(kHaTokenKey, *settings.accessToken.value);
                 reapplyHomeAssistant(ctx, haChanged);
                 if (!tokenSaved) {
                     return sendError(out, 503, "storage_unavailable",
                                      "The settings were saved but the token couldn't be stored; send it again");
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
