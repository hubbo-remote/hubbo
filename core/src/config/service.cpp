#include "hubbo/config/service.h"

#include "hubbo/config/profile.h"

#include <algorithm>

#include "hubbo/domain/ids.h"

namespace hubbo::config {

namespace {

constexpr size_t kMaxSlug = 28;  // api/openapi.yaml: ^(act|dev)_[a-z0-9_]{1,28}$

std::string slug(const std::string &name) {
    std::string out;
    for (char c : name) {
        const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) {
            out += lower;
        } else if (!out.empty() && out.back() != '_') {
            out += '_';
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.size() > kMaxSlug) out.resize(kMaxSlug);
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

template <typename T>
const T *findIn(const std::vector<T> &items, std::string_view id) {
    const int i = indexOfId(items, id);
    return i < 0 ? nullptr : &items[static_cast<size_t>(i)];
}

// Issues whose path starts with `from` are re-rooted at `to` (the request
// body). Others keep their config-rooted path.
ValidationReport reroot(const ValidationReport &report, const std::string &from, const std::string &to) {
    ValidationReport out;
    for (const auto &issue : report.issues()) {
        const bool inside = !from.empty() && issue.path.compare(0, from.size(), from) == 0 &&
                            (issue.path.size() == from.size() || issue.path[from.size()] == '/');
        out.add(issue.code, inside ? to + issue.path.substr(from.size()) : issue.path);
    }
    return out;
}

Outcome failure(ErrorCode code) {
    Outcome o;
    o.code = code;
    return o;
}

Outcome conflict(uint32_t current) {
    Outcome o = failure(ErrorCode::RevisionConflict);
    o.currentRevision = current;
    return o;
}

Outcome invalid(ErrorCode code, const char *path) {
    Outcome o = failure(ErrorCode::ValidationFailed);
    o.issues.add(code, path);
    return o;
}

// Where an action sits; the JSON Pointer is only built for matches.
struct ActionRef {
    const std::string &owner;      // activity id, "" for the global bindings
    const std::string &ownerName;  // activity name, "" for the global bindings
    const char *section;       // "/startup", "/shutdown", "/bindings"
    const std::string *trigger;  // bindings only
    size_t index;

    Referrer referrer() const {
        std::string path = section;
        if (trigger != nullptr) path += "/" + jsonPointerToken(*trigger);
        return {owner, path + "/" + std::to_string(index), ownerName};
    }
};

template <typename F>
void forEachAction(const ConfigSpec &config, F &&visit) {
    static const std::string kGlobal;
    for (const auto &a : config.activities) {
        for (size_t i = 0; i < a.startup.size(); i++) {
            visit(ActionRef{a.id, a.name, "/startup", nullptr, i}, a.startup[i]);
        }
        for (size_t i = 0; i < a.shutdown.size(); i++) {
            visit(ActionRef{a.id, a.name, "/shutdown", nullptr, i}, a.shutdown[i]);
        }
        for (const auto &b : a.bindings) {
            for (size_t i = 0; i < b.actions.size(); i++) {
                visit(ActionRef{a.id, a.name, "/bindings", &b.trigger, i}, b.actions[i]);
            }
        }
    }
    for (const auto &b : config.globalBindings) {
        for (size_t i = 0; i < b.actions.size(); i++) {
            visit(ActionRef{kGlobal, kGlobal, "/bindings", &b.trigger, i}, b.actions[i]);
        }
    }
}

template <typename Match>
std::vector<Referrer> referrersOf(const ConfigSpec &config, Match match) {
    std::vector<Referrer> out;
    forEachAction(config, [&](const ActionRef &ref, const ActionSpec &a) {
        if (match(ref, a)) out.push_back(ref.referrer());
    });
    return out;
}

// Clears fields that don't apply to their type (they aren't stored, so
// they must not live in the running config either).
void dropInapplicableFields(ConfigSpec *config) {
    for (auto &d : config->devices) {
        if (d.provider.type == kBleHidType) {
            d.provider.entityId.clear();
            d.provider.uniqueId.clear();
        }
        if (d.provider.type != kHomeAssistantType) d.haCapabilities.reset();
    }
    for (auto &i : config->integrations) {
        if (i.type != kHomeAssistantType) {
            i.url.clear();
            i.verifyTls = true;
        }
        if (i.type != kBleHidType) i.deviceName.clear();
    }
}

// Every object gets `generation` as its revision (restore, reset, defaults),
// or only those without one.
void stampRevisions(ConfigSpec *config, uint32_t generation, bool all) {
    auto stamp = [&](uint32_t &revision) {
        if (all || revision == 0) revision = generation;
    };
    for (auto &a : config->activities) stamp(a.revision);
    for (auto &d : config->devices) stamp(d.revision);
    for (auto &i : config->integrations) stamp(i.revision);
    stamp(config->globalBindingsRevision);
}

ConfigSpec emptyConfig() {
    ConfigSpec c;
    ensureBuiltinIntegrations(&c);
    return c;
}

}  // namespace

const char *configStateName(ConfigState state) {
    switch (state) {
        case ConfigState::Fresh: return "fresh";
        case ConfigState::Ok: return "ok";
        case ConfigState::Blocked: return "blocked";
        case ConfigState::StorageUnavailable: return "storage_unavailable";
    }
    return "blocked";
}

ConfigService::ConfigService(storage::RecordStore &records, const AdapterSet &adapters, Engine::Store &store,
                             Migration migrate)
    : records_(records), adapters_(adapters), store_(store), migrate_(migrate) {}

LoadReport ConfigService::load(const ConfigSpec &defaults) {
    if (&defaults != &defaults_) defaults_ = defaults;
    LoadReport report;
    // Runs nothing (Blocked, StorageUnavailable): publish an empty config.
    auto runNothing = [&](ConfigState state, ErrorCode reason) {
        state_ = state;
        report.state = state;
        report.reason = reason;
        config_ = emptyConfig();
        if (!runningNothing_) {  // don't republish an identical empty snapshot
            ValidationReport ignored;
            std::unique_ptr<CompiledConfig> empty = compileConfig(config_, adapters_, &ignored);
            if (store_.publish(empty) == ErrorCode::Ok) runningNothing_ = true;
        }
        load_ = report;
        return report;
    };

    storage::Record record;
    const ErrorCode loaded = records_.health() == storage::Health::Unavailable
                                 ? ErrorCode::StorageFailed
                                 : records_.load(kRecordName, &record);
    if (loaded == ErrorCode::StorageFailed) return runNothing(ConfigState::StorageUnavailable, ErrorCode::StorageFailed);
    // Absence needs proof: a record that exists but has no intact copy is
    // never a first boot. Blocked (untouched) until a restore or reset.
    if (loaded == ErrorCode::Corrupt) return runNothing(ConfigState::Blocked, ErrorCode::Corrupt);

    if (loaded == ErrorCode::NotFound) {
        // Fresh: run the defaults and save them as generation 1.
        report.state = ConfigState::Fresh;
        state_ = ConfigState::Fresh;
        config_ = {};
        const Outcome saved = commit(defaults, {}, "", CommitKind::Restore);
        if (saved.ok()) {
            report.saved = true;
            report.generation = config_.revision;
        } else if (saved.code == ErrorCode::ValidationFailed) {
            report.issues = saved.issues;  // bad defaults: run nothing
            return runNothing(ConfigState::Blocked, ErrorCode::ValidationFailed);
        } else {
            // Couldn't save (e.g. read-only): run the defaults unsaved, built
            // by the same pipeline (a dry run) and published as parsed.
            report.reason = saved.code;
            Built built;
            const Outcome dry = commit(defaults, {}, "", CommitKind::Restore, /*dryRun=*/true, &built);
            if (dry.code == ErrorCode::ValidationFailed) {
                report.issues = dry.issues;  // bad defaults: run nothing
                return runNothing(ConfigState::Blocked, ErrorCode::ValidationFailed);
            }
            if (dry.ok() && store_.publish(built.compiled) == ErrorCode::Ok) {
                runningNothing_ = false;
                config_ = std::move(built.parsed);
            }
        }
        load_ = report;
        return report;
    }

    // An intact record: run it if it's usable, else Blocked (untouched).
    report.generation = record.generation;
    ConfigSpec next;
    const ParseResult parsed = parseConfig(record.payload, &next, migrate_);
    if (parsed.code != ErrorCode::Ok) {
        report.issues = parsed.report;
        return runNothing(ConfigState::Blocked, parsed.code);
    }
    next.revision = static_cast<uint32_t>(record.generation);
    const bool migrated = parsed.schemaVersion != kConfigFileVersion;
    if (migrated) report.migratedFrom = parsed.schemaVersion;
    const size_t integrationsBefore = next.integrations.size();
    ensureBuiltinIntegrations(&next);
    const bool upgraded = migrated || next.integrations.size() != integrationsBefore;

    ValidationReport issues;
    std::unique_ptr<CompiledConfig> compiled = compileConfig(next, adapters_, &issues);
    if (!compiled) {
        report.issues = issues;
        return runNothing(ConfigState::Blocked, ErrorCode::ValidationFailed);
    }
    if (store_.publish(compiled) == ErrorCode::Ok) runningNothing_ = false;
    config_ = next;
    state_ = ConfigState::Ok;
    report.state = ConfigState::Ok;
    if (upgraded) {
        // Saved once, as a new generation (the commit tells the listener).
        const Outcome saved = commit(std::move(next), {}, "", CommitKind::Edit);
        report.saved = saved.ok();
        if (saved.ok()) {
            report.generation = config_.revision;
        } else {
            report.reason = saved.code;
        }
    }
    if (!report.saved && listener_ != nullptr) listener_->configSaved(config_);  // the intact record
    load_ = report;
    return report;
}

ConfigState ConfigService::state() const {
    if (state_ == ConfigState::Blocked || state_ == ConfigState::StorageUnavailable) return state_;
    return records_.health() == storage::Health::Available ? state_ : ConfigState::StorageUnavailable;
}

ConfigState ConfigService::refresh(uint64_t nowMs) {
    if (state_ != ConfigState::StorageUnavailable) return state();
    const storage::Health health = records_.health();
    if (health != retryHealth_) {  // a health transition retries at once
        retryHealth_ = health;
        retryDelayMs_ = 0;
    }
    if (health != storage::Health::Available) return state();
    if (retryDelayMs_ != 0 && nowMs < retryAtMs_) return state();
    // Same rules as boot: an unreadable slot fails again (StorageFailed),
    // and only a positively empty store starts from the defaults.
    load(defaults_);
    if (state_ == ConfigState::StorageUnavailable) {
        retryDelayMs_ = retryDelayMs_ == 0 ? kFirstRetryMs : std::min<uint64_t>(retryDelayMs_ * 2, kMaxRetryMs);
        retryAtMs_ = nowMs + retryDelayMs_;
    } else {
        retryDelayMs_ = 0;
    }
    return state();
}

ErrorCode ConfigService::editable() const {
    switch (state()) {
        case ConfigState::Blocked: return ErrorCode::ConfigBlocked;
        case ConfigState::StorageUnavailable: return ErrorCode::StorageFailed;
        default: return ErrorCode::Ok;
    }
}

ErrorCode ConfigService::blockedRecord(std::string *payload, uint64_t *generation) {
    if (state_ != ConfigState::Blocked) return ErrorCode::NotFound;
    storage::Record record;
    const ErrorCode loaded = records_.load(kRecordName, &record);
    if (loaded != ErrorCode::Ok) return loaded;  // NotFound (bad defaults), Corrupt, StorageFailed
    payload->swap(record.payload);
    if (generation != nullptr) *generation = record.generation;
    return ErrorCode::Ok;
}

uint32_t ConfigService::generation() const {
    return state_ == ConfigState::Blocked ? static_cast<uint32_t>(load_.generation) : config_.revision;
}

Outcome ConfigService::commit(ConfigSpec next, Outcome outcome, const std::string &issuePrefix, CommitKind kind,
                              bool dryRun, Built *built) {
    // One step order for every edit: editable, (revision: by the caller,
    // after this check), build, round trip, compile, save, publish.
    const ErrorCode allowed = editable();
    const bool restoring = kind == CommitKind::Restore;
    const bool stampAll = restoring;
    if (allowed == ErrorCode::StorageFailed && !dryRun) return failure(allowed);
    if (allowed == ErrorCode::ConfigBlocked && !restoring) return failure(allowed);

    // The store owns generations: ask it, never cache one.
    uint64_t next64 = 0;
    if (records_.nextGeneration(kRecordName, &next64) != ErrorCode::Ok) {
        if (!dryRun) return failure(ErrorCode::StorageFailed);
        next64 = std::max<uint64_t>(load_.generation, config_.revision) + 1;  // a dry run still validates
    }
    const auto generation = static_cast<uint32_t>(next64);
    ensureBuiltinIntegrations(&next);
    dropInapplicableFields(&next);
    stampRevisions(&next, generation, stampAll);
    next.revision = generation;

    // The hub runs what's saved: serialize, parse the bytes back, and
    // compile and publish the parsed result.
    const std::string text = serializeConfig(next);
    if (text.size() > kMaxConfigFileBytes) return failure(ErrorCode::LimitReached);
    ConfigSpec parsed;
    if (parseConfig(text, &parsed).code != ErrorCode::Ok) return failure(ErrorCode::Internal);
    ValidationReport issues;
    std::unique_ptr<CompiledConfig> compiled = compileConfig(parsed, adapters_, &issues);
    if (!compiled || !issues.ok()) {
        outcome.code = ErrorCode::ValidationFailed;
        outcome.issues = reroot(issues, issuePrefix, "");
        return outcome;
    }

    // Save, then publish, so the engine never runs a config that isn't
    // saved. This task is the only writer, so once there's room to publish
    // (old snapshots reclaimable), the publish after the save can't fail.
    // Restore/reset never start an activity; only deleting the active one
    // makes the engine switch to the next (design part 6).
    if (kind == CommitKind::DeleteActivity) compiled->deletedActivity.assign(outcome.id);
    outcome.revision = generation;  // the changed object's (or, on restore, every object's)
    if (dryRun) {
        if (built != nullptr) {
            built->parsed = std::move(parsed);
            built->compiled = std::move(compiled);
        }
        outcome.code = ErrorCode::Ok;
        return outcome;
    }
    if (store_.reclaim() >= limits::kMaxRetiredSnapshots) return failure(ErrorCode::Busy);
    if (records_.save(kRecordName, generation, text) != ErrorCode::Ok) return failure(ErrorCode::StorageFailed);
    config_ = std::move(parsed);
    state_ = ConfigState::Ok;
    if (store_.publish(compiled) != ErrorCode::Ok) return failure(ErrorCode::Internal);
    runningNothing_ = false;
    if (listener_ != nullptr) listener_->configSaved(config_);
    outcome.code = ErrorCode::Ok;
    return outcome;
}

Outcome ConfigService::restore(ConfigSpec config, bool dryRun) {
    return commit(std::move(config), {}, "", CommitKind::Restore, dryRun);
}

const ActivitySpec *ConfigService::findActivity(std::string_view id) const { return findIn(config_.activities, id); }
const DeviceSpec *ConfigService::findDevice(std::string_view id) const { return findIn(config_.devices, id); }
const IntegrationSpec *ConfigService::findIntegration(std::string_view id) const {
    return findIn(config_.integrations, id);
}

std::string ConfigService::uniqueId(const char *prefix, const std::string &name, const char *fallback) const {
    std::string base = slug(name);
    if (base.empty()) base = fallback;
    for (unsigned n = 1;; n++) {
        std::string candidate = base;
        if (n > 1) {
            const std::string suffix = "_" + std::to_string(n);
            if (candidate.size() + suffix.size() > kMaxSlug) candidate.resize(kMaxSlug - suffix.size());
            candidate += suffix;
        }
        const std::string id = prefix + candidate;
        if (indexOfId(config_.activities, id) < 0 && indexOfId(config_.devices, id) < 0) return id;
    }
}

template <typename T>
Outcome ConfigService::add(std::vector<T> ConfigSpec::*member, size_t limit, const char *idPrefix,
                           const char *fallbackName, const char *section, T item) {
    if (editable() != ErrorCode::Ok) return failure(editable());
    if ((config_.*member).size() >= limit) return failure(ErrorCode::LimitReached);
    item.id = uniqueId(idPrefix, item.name, fallbackName);
    item.revision = 0;  // commit stamps it with the new generation
    Outcome outcome;
    outcome.id = item.id;
    ConfigSpec next = config_;
    const std::string prefix = std::string(section) + "/" + std::to_string((next.*member).size());
    (next.*member).push_back(std::move(item));
    return commit(std::move(next), std::move(outcome), prefix);
}

template <typename T>
Outcome ConfigService::replace(std::vector<T> ConfigSpec::*member, size_t index, const char *section, T item) {
    item.revision = 0;  // commit stamps it with the new generation
    Outcome outcome;
    outcome.id = item.id;
    ConfigSpec next = config_;
    (next.*member)[index] = std::move(item);
    return commit(std::move(next), std::move(outcome), std::string(section) + "/" + std::to_string(index));
}

template <typename T>
Outcome ConfigService::erase(std::vector<T> ConfigSpec::*member, size_t index, CommitKind kind) {
    Outcome outcome;
    outcome.id = (config_.*member)[index].id;
    ConfigSpec next = config_;
    (next.*member).erase((next.*member).begin() + static_cast<std::ptrdiff_t>(index));
    return commit(std::move(next), std::move(outcome), "", kind);
}

Outcome ConfigService::createActivity(ActivitySpec activity) {
    return add(&ConfigSpec::activities, limits::kMaxActivities, "act_", "activity", "/activities",
               std::move(activity));
}

Outcome ConfigService::replaceActivity(std::string_view id, ActivitySpec activity, uint32_t baseRevision) {
    if (editable() != ErrorCode::Ok) return failure(editable());
    const int index = indexOfId(config_.activities, id);
    if (index < 0) return failure(ErrorCode::NotFound);
    if (!activity.id.empty() && activity.id != id) return invalid(ErrorCode::InvalidId, "/id");
    const uint32_t current = config_.activities[static_cast<size_t>(index)].revision;
    if (baseRevision != current) return conflict(current);
    activity.id = std::string(id);
    return replace(&ConfigSpec::activities, static_cast<size_t>(index), "/activities", std::move(activity));
}

Outcome ConfigService::deleteActivity(std::string_view id, uint32_t baseRevision) {
    if (editable() != ErrorCode::Ok) return failure(editable());
    const int index = indexOfId(config_.activities, id);
    if (index < 0) return failure(ErrorCode::NotFound);
    const uint32_t current = config_.activities[static_cast<size_t>(index)].revision;
    if (baseRevision != current) return conflict(current);

    Outcome refused = failure(ErrorCode::Referenced);
    refused.referrers = referrersOf(config_, [&](const ActionRef &ref, const ActionSpec &a) {
        if (ref.owner == id || a.integrationId != kSystemIntegrationId || a.commandId != "activate") return false;
        for (const auto &p : a.params) {
            if (p.name == "activity_id" && p.value.kind == ParamLiteral::Kind::String && p.value.string == id) {
                return true;
            }
        }
        return false;
    });
    if (!refused.referrers.empty()) return refused;
    return erase(&ConfigSpec::activities, static_cast<size_t>(index), CommitKind::DeleteActivity);
}

ValidationReport ConfigService::validateActivity(const ActivitySpec &activity) const {
    if (!activity.id.empty()) return hubbo::validateActivity(activity, config_, adapters_);
    // A draft (not yet created): validate it under the id it would get.
    ActivitySpec draft = activity;
    draft.id = uniqueId("act_", activity.name, "activity");
    return hubbo::validateActivity(draft, config_, adapters_);
}

Outcome ConfigService::replaceGlobalBindings(std::vector<BindingSpec> bindings, uint32_t baseRevision) {
    if (editable() != ErrorCode::Ok) return failure(editable());
    if (baseRevision != config_.globalBindingsRevision) return conflict(config_.globalBindingsRevision);
    Outcome outcome;
    ConfigSpec next = config_;
    next.globalBindings = std::move(bindings);
    next.globalBindingsRevision = 0;  // commit stamps it
    Outcome result = commit(std::move(next), std::move(outcome), "");
    result.issues = reroot(result.issues, "/global_bindings", "/bindings");
    return result;
}

namespace {

// A created or updated device's own rules, checked before it's built so a
// refusal is a 422 and a failed read-back stays truly internal. Kinds come
// from kinds.json, but only the device being edited is checked: a saved
// device whose kind an update dropped never blocks other edits.
Outcome checkDevice(const DeviceSpec &device, bool kindChanged) {
    Outcome outcome = failure(ErrorCode::ValidationFailed);
    if (kindChanged && !builtinProfiles().hasKind(device.kind)) outcome.issues.add(ErrorCode::UnknownKind, "/kind");
    const bool bluetooth = device.provider.type == kBleHidType;
    if (bluetooth && !device.profile) outcome.issues.add(ErrorCode::Required, "/profile");
    if (!bluetooth && device.profile) outcome.issues.add(ErrorCode::UnknownField, "/profile");
    if (outcome.issues.ok()) outcome.code = ErrorCode::Ok;
    return outcome;
}

}  // namespace

Outcome ConfigService::createDevice(DeviceSpec device) {
    if (editable() != ErrorCode::Ok) return failure(editable());
    const Outcome checked = checkDevice(device, true);
    if (checked.code != ErrorCode::Ok) return checked;
    return add(&ConfigSpec::devices, limits::kMaxDevices, "dev_", "device", "/devices", std::move(device));
}

Outcome ConfigService::updateDevice(std::string_view id, const DeviceUpdate &update) {
    if (editable() != ErrorCode::Ok) return failure(editable());
    const int index = indexOfId(config_.devices, id);
    if (index < 0) return failure(ErrorCode::NotFound);
    DeviceSpec device = config_.devices[static_cast<size_t>(index)];
    if (update.revision != device.revision) return conflict(device.revision);
    if (update.provider && update.provider->type != device.provider.type) {
        return invalid(ErrorCode::IntegrationMismatch, "/provider/type");  // a device stays on its platform
    }
    if (update.name) device.name = *update.name;
    if (update.kind) device.kind = *update.kind;
    if (update.provider) device.provider = *update.provider;
    if (update.haCapabilities) device.haCapabilities = *update.haCapabilities;
    const Outcome checked = checkDevice(device, update.kind.has_value());
    if (checked.code != ErrorCode::Ok) return checked;
    return replace(&ConfigSpec::devices, static_cast<size_t>(index), "/devices", std::move(device));
}

std::vector<Referrer> ConfigService::deviceReferrers(std::string_view id) const {
    return referrersOf(config_, [&](const ActionRef &, const ActionSpec &a) { return a.deviceId && *a.deviceId == id; });
}

Outcome ConfigService::deleteDevice(std::string_view id, uint32_t baseRevision) {
    if (editable() != ErrorCode::Ok) return failure(editable());
    const int index = indexOfId(config_.devices, id);
    if (index < 0) return failure(ErrorCode::NotFound);
    const uint32_t current = config_.devices[static_cast<size_t>(index)].revision;
    if (baseRevision != current) return conflict(current);
    Outcome refused = failure(ErrorCode::DeviceInUse);
    refused.referrers = deviceReferrers(id);
    if (!refused.referrers.empty()) return refused;
    return erase(&ConfigSpec::devices, static_cast<size_t>(index));
}

Outcome ConfigService::replaceIntegration(std::string_view id, IntegrationSpec integration, uint32_t baseRevision) {
    if (editable() != ErrorCode::Ok) return failure(editable());
    const int index = indexOfId(config_.integrations, id);
    if (index < 0) return failure(ErrorCode::NotFound);
    const IntegrationSpec &current = config_.integrations[static_cast<size_t>(index)];
    if (integration.id != id) return invalid(ErrorCode::IntegrationMismatch, "/id");
    if (integration.type != current.type) return invalid(ErrorCode::IntegrationMismatch, "/type");
    if (baseRevision != current.revision) return conflict(current.revision);
    return replace(&ConfigSpec::integrations, static_cast<size_t>(index), "/integrations", std::move(integration));
}

Outcome ConfigService::compileCommand(std::string_view deviceId, std::string_view commandId,
                                      const std::vector<ParamInput> &params, CompiledCommand *out) const {
    Outcome outcome;
    outcome.id = std::string(deviceId);
    const ErrorCode code = compileDeviceCommand(config_, adapters_, deviceId, commandId, params, out, &outcome.issues);
    switch (code) {
        case ErrorCode::Ok: break;
        case ErrorCode::UnknownCommand:
        case ErrorCode::ValidationFailed: outcome.code = ErrorCode::ValidationFailed; break;
        default: outcome.code = code;  // NotFound, IntegrationDisabled, other resolver errors
    }
    return outcome;
}

}  // namespace hubbo::config
