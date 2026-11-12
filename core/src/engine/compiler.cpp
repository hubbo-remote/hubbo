#include "hubbo/engine/compiler.h"

#include <algorithm>
#include <string_view>

#include "hubbo/domain/ids.h"
#include "hubbo/domain/system.h"
#include "hubbo/domain/trigger.h"

namespace hubbo {

void ValidationReport::add(ErrorCode code, std::string path) {
    if (issues_.size() >= limits::kMaxValidationIssues) {
        dropped_++;
        return;
    }
    issues_.push_back({code, std::move(path)});
}

bool ValidationReport::has(ErrorCode code, const std::string &path) const {
    for (const auto &issue : issues_) {
        if (issue.code == code && issue.path == path) return true;
    }
    return false;
}

std::string jsonPointerToken(const std::string &token) {
    std::string out;
    out.reserve(token.size());
    for (char c : token) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

namespace {

// Where a command runs, and what its effect allows there: a Hold only in a
// binding (paired with its release there), a Wait only in a startup or
// shutdown, so a direct execute runs neither. Ok, HoldNotAllowed or
// SequenceOnly.
enum class Place { Sequence, Binding, Direct };
ErrorCode misplacedEffect(const CommandSpec *command, Place place) {
    if (command == nullptr) return ErrorCode::Ok;
    if (command->effect == CommandEffect::Hold && place != Place::Binding) return ErrorCode::HoldNotAllowed;
    if (command->effect == CommandEffect::Wait && place != Place::Sequence) return ErrorCode::SequenceOnly;
    return ErrorCode::Ok;
}

// Looks up `commandId` in `catalog` and resolves `params` against it into
// `out` (command, commandIndex, params, paramsPresent). On failure, adds
// the issue under `path` (/command_id or /params/<name>) and returns false.
bool resolveCommand(const CommandCatalog &catalog, std::string_view commandId, const std::vector<ParamInput> &params,
                    const RefResolver *refs, const std::string &path, CompiledAction *out, ValidationReport &report) {
    const int command = catalog.find(commandId);
    if (command < 0) {
        report.add(ErrorCode::UnknownCommand, path + "/command_id");
        return false;
    }
    const CommandSpec &spec = catalog.items[command];
    std::string badParam;
    const ErrorCode code = resolveParams(spec, params, out->params, &badParam, refs, &out->paramsPresent);
    if (code != ErrorCode::Ok) {
        report.add(code, badParam.empty() ? path + "/params" : path + "/params/" + jsonPointerToken(badParam));
        return false;
    }
    out->command = &spec;
    out->commandIndex = static_cast<uint16_t>(command);
    return true;
}

struct DeviceInfo {
    bool valid = false;
    bool disabled = false;  // its integration is disabled: actions compile to Unavailable
    int adapter = -1;
    CommandCatalog catalog;
};

class Compiler : public RefResolver {
public:
    Compiler(const AdapterSet &adapters, ValidationReport &report, CompiledConfig *out)
        : adapters_(adapters), report_(report), out_(out) {}

    // Devices first: actions resolve against them, each through
    // resolveIntegration(). Device problems go to `sink` (defaults to the
    // main report).
    void devices(const ConfigSpec &config, const std::string &prefix, ValidationReport *sink = nullptr) {
        ValidationReport &report = sink != nullptr ? *sink : report_;
        const std::vector<DeviceSpec> &devices = config.devices;
        config_ = &config;
        info_.assign(devices.size(), DeviceInfo{});
        if (devices.size() > limits::kMaxDevices) {
            report.add(ErrorCode::TooManyDevices, prefix);
            return;
        }
        size_t bluetooth = 0;
        for (size_t i = 0; i < devices.size(); i++) {
            const DeviceSpec &d = devices[i];
            const std::string path = prefix + "/" + std::to_string(i);
            bool ok = true;
            // One host per Bluetooth device, and the radio holds kMaxBleDevices bonds.
            if (d.provider.type == kBleHidType && ++bluetooth > limits::kMaxBleDevices) {
                report.add(ErrorCode::TooManyDevices, path + "/integration_id");
                ok = false;
            }
            if (!isValidId(d.id)) {
                report.add(ErrorCode::InvalidId, path + "/id");
                ok = false;
            } else if (indexOfId(devices, d.id, i) >= 0) {
                report.add(ErrorCode::DuplicateId, path + "/id");
                ok = false;
            }
            if (!isValidName(d.name)) {
                report.add(ErrorCode::InvalidName, path + "/name");
                ok = false;
            }
            const IntegrationResolution resolved = resolveIntegration(config, adapters_, d);
            switch (resolved.error) {
                case ErrorCode::Ok:
                case ErrorCode::IntegrationDisabled:
                    break;
                case ErrorCode::IntegrationMismatch:
                    report.add(ErrorCode::IntegrationMismatch, path + "/provider/type");
                    ok = false;
                    break;
                default:
                    report.add(isValidId(d.integrationId) ? resolved.error : ErrorCode::InvalidId,
                               path + "/integration_id");
                    ok = false;
            }
            const std::string ref = providerRef(d);
            if (ref.empty()) {
                report.add(ErrorCode::Required, path + "/provider");
                ok = false;
            } else if (ref.size() > limits::kMaxProviderRefLength) {
                report.add(ErrorCode::TooLong, path + "/provider");
                ok = false;
            }
            if (!ok) continue;
            info_[i].valid = true;
            info_[i].disabled = resolved.error == ErrorCode::IntegrationDisabled;
            info_[i].adapter = resolved.adapterIndex;
            info_[i].catalog = adapters_.at(static_cast<size_t>(resolved.adapterIndex))->commandsFor(d);
        }
        if (out_ != nullptr) {
            out_->devices.resize(devices.size());
            for (size_t i = 0; i < devices.size(); i++) {
                out_->devices[i].id.assign(devices[i].id);
                out_->devices[i].providerRef.assign(providerRef(devices[i]));
            }
        }
    }

    // Integration ids valid and unique; only "system" is of type system.
    // (Whether an adapter exists is checked per device by the resolver.)
    void integrations(const std::vector<IntegrationSpec> &integrations, const std::string &prefix) {
        if (integrations.size() > limits::kMaxAdapters + 2) {
            report_.add(ErrorCode::TooLong, prefix);
            return;
        }
        for (size_t i = 0; i < integrations.size(); i++) {
            const IntegrationSpec &in = integrations[i];
            const std::string path = prefix + "/" + std::to_string(i);
            if (!isValidId(in.id)) {
                report_.add(ErrorCode::InvalidId, path + "/id");
            } else if (indexOfId(integrations, in.id, i) >= 0) {
                report_.add(ErrorCode::DuplicateId, path + "/id");
            } else if ((in.id == kSystemIntegrationId) != (in.type == kSystemType)) {
                report_.add(ErrorCode::IntegrationMismatch, path + "/type");
            }
            if (!isValidName(in.name)) report_.add(ErrorCode::InvalidName, path + "/name");
        }
    }

    // The ids system.activate may reference, index = compiled activity index.
    void activityIds(std::vector<std::string> ids) { activityIds_ = std::move(ids); }

    int activityIndex(std::string_view id) const override {
        for (size_t i = 0; i < activityIds_.size(); i++) {
            if (activityIds_[i] == id) return static_cast<int>(i);
        }
        return -1;
    }

    // Duplicate ids are the caller's check (it knows the siblings).
    void activity(const ActivitySpec &a, const std::string &prefix, CompiledActivity *out) {
        if (!isValidId(a.id)) {
            report_.add(ErrorCode::InvalidId, prefix + "/id");
        }
        if (!isValidName(a.name)) {
            report_.add(ErrorCode::InvalidName, prefix + "/name");
        }
        ActionSpan startup, shutdown;
        sequence(a.startup, /*isBinding=*/false, prefix + "/startup", &startup);
        sequence(a.shutdown, /*isBinding=*/false, prefix + "/shutdown", &shutdown);
        TriggerTable table{};
        bindings(a.bindings, prefix + "/bindings", &table);
        if (out != nullptr) {
            out->id.assign(a.id);
            out->name.assign(a.name);
            out->revision = a.revision;
            out->startup = startup;
            out->shutdown = shutdown;
            out->table = table;
        }
    }

    // Hold commands (e.g. key_down) are allowed only in "<button>:down"
    // bindings, and the same scope's "<button>:up" binding must contain the
    // matching release (same device, release command and params). This is
    // early feedback for the user only: the engine's hold ledger is what
    // guarantees the release at runtime (design section 5), e.g. when an
    // activity overrides only the :up of a global hold.
    void bindings(const std::vector<BindingSpec> &bindings, const std::string &prefix, TriggerTable *table) {
        bool seen[kTriggerCount] = {};
        const std::string *paths[kTriggerCount] = {};
        std::vector<std::string> pathStore(bindings.size());
        for (size_t b = 0; b < bindings.size(); b++) {
            const BindingSpec &binding = bindings[b];
            pathStore[b] = prefix + "/" + jsonPointerToken(binding.trigger);
            const std::string &path = pathStore[b];
            Trigger trigger;
            if (!parseTrigger(binding.trigger, &trigger)) {
                report_.add(ErrorCode::InvalidTrigger, path);
                continue;
            }
            if (seen[triggerIndex(trigger)]) {
                report_.add(ErrorCode::DuplicateTrigger, path);
                continue;
            }
            seen[triggerIndex(trigger)] = true;
            paths[triggerIndex(trigger)] = &path;
            ActionSpan span;
            sequence(binding.actions, /*isBinding=*/true, path, &span);
            table->spans[static_cast<size_t>(trigger.button)][static_cast<size_t>(trigger.kind)] = span;
        }
        for (size_t t = 0; t < kTriggerCount; t++) {
            if (paths[t] == nullptr) continue;
            const auto button = static_cast<remote::Button>(t / remote::kEventKindCount);
            const auto kind = static_cast<remote::EventKind>(t % remote::kEventKindCount);
            if (kind == remote::EventKind::Down) {
                unpairedHolds(*table, button, *paths[t]);
                continue;
            }
            const ActionSpan &span = table->at(button, kind);
            for (uint8_t i = 0; i < span.count; i++) {
                const CompiledAction &hold = actions_[span.offset + i];
                if (hold.command == nullptr || hold.command->effect != CommandEffect::Hold) continue;
                report_.add(ErrorCode::HoldNotAllowed, *paths[t] + "/" + std::to_string(i) + "/command_id");
            }
        }
    }

    void unpairedHolds(const TriggerTable &table, remote::Button button, const std::string &path) {
        const ActionSpan &down = table.at(button, remote::EventKind::Down);
        const ActionSpan &up = table.at(button, remote::EventKind::Up);
        for (uint8_t i = 0; i < down.count; i++) {
            const CompiledAction &hold = actions_[down.offset + i];
            if (hold.command == nullptr || hold.command->effect != CommandEffect::Hold || hold.release == nullptr) {
                continue;
            }
            // Pairing is a property of the bindings: a disabled integration's
            // actions (Unavailable) pair the same way, so disabling never fails.
            bool paired = false;
            for (uint8_t j = 0; j < up.count && !paired; j++) {
                const CompiledAction &r = actions_[up.offset + j];
                const bool adapterAction = r.op == ActionOp::Adapter || r.op == ActionOp::Unavailable;
                if (!adapterAction || r.adapterIndex != hold.adapterIndex ||
                    r.deviceIndex != hold.deviceIndex || r.commandIndex != hold.releaseIndex) {
                    continue;
                }
                paired = true;
                for (uint8_t p = 0; p < hold.release->paramCount; p++) {
                    const ParamValue &a = hold.params[p];
                    const ParamValue &b = r.params[p];
                    if (a.type != b.type || a.integer != b.integer || a.number != b.number) paired = false;
                }
            }
            if (!paired) report_.add(ErrorCode::UnpairedHold, path + "/" + std::to_string(i) + "/command_id");
        }
    }

    void finish() {
        if (actions_.size() > limits::kMaxCompiledActions) {
            report_.add(ErrorCode::CapacityExceeded, "");
        }
        if (out_ != nullptr) {
            actions_.shrink_to_fit();  // lives as long as the snapshot
            out_->actions = std::move(actions_);
        }
    }

private:
    // system.activate/off are allowed only as the last action of a binding;
    // hold commands only in bindings (checked in bindings()).
    void sequence(const std::vector<ActionSpec> &actions, bool isBinding, const std::string &prefix,
                  ActionSpan *span) {
        *span = ActionSpan{};
        if (actions.size() > limits::kMaxSequenceLength) {
            report_.add(ErrorCode::TooManyActions, prefix);
            return;
        }
        const size_t offset = actions_.size();
        for (size_t i = 0; i < actions.size(); i++) {
            CompiledAction compiled;
            const std::string path = prefix + "/" + std::to_string(i);
            if (!action(actions[i], path, &compiled)) {
                continue;
            }
            const bool transition = compiled.op == ActionOp::Activate || compiled.op == ActionOp::Off;
            if (transition && (!isBinding || i + 1 != actions.size())) {
                report_.add(ErrorCode::MisplacedTransition, path + "/command_id");
                continue;
            }
            const ErrorCode misplaced = misplacedEffect(compiled.command, isBinding ? Place::Binding : Place::Sequence);
            if (misplaced != ErrorCode::Ok) {
                report_.add(misplaced, path + "/command_id");
                continue;
            }
            actions_.push_back(compiled);
        }
        if (actions_.size() <= limits::kMaxCompiledActions) {
            span->offset = static_cast<uint16_t>(offset);
            span->count = static_cast<uint8_t>(actions_.size() - offset);
        }
    }

    bool action(const ActionSpec &a, const std::string &path, CompiledAction *out) {
        if (!isValidId(a.integrationId)) {
            report_.add(ErrorCode::InvalidId, path + "/integration_id");
            return false;
        }
        if (!isValidCommandId(a.commandId)) {
            report_.add(ErrorCode::InvalidCommandId, path + "/command_id");
            return false;
        }

        CommandCatalog catalog;
        if (a.integrationId == kSystemIntegrationId) {
            if (a.deviceId.has_value()) {
                report_.add(ErrorCode::DeviceNotAllowed, path + "/device_id");
                return false;
            }
            catalog = systemCatalog();
        } else {
            if (indexOfId(config_->integrations, a.integrationId) < 0) {
                report_.add(ErrorCode::UnknownIntegration, path + "/integration_id");
                return false;
            }
            if (!a.deviceId.has_value()) {
                report_.add(ErrorCode::DeviceRequired, path + "/device_id");
                return false;
            }
            if (!isValidId(*a.deviceId)) {
                report_.add(ErrorCode::InvalidId, path + "/device_id");
                return false;
            }
            const int device = indexOfId(config_->devices, *a.deviceId);
            if (device < 0 || !info_[static_cast<size_t>(device)].valid) {
                report_.add(ErrorCode::UnknownDevice, path + "/device_id");
                return false;
            }
            const DeviceInfo &info = info_[static_cast<size_t>(device)];
            if (config_->devices[static_cast<size_t>(device)].integrationId != a.integrationId) {
                report_.add(ErrorCode::IntegrationMismatch, path + "/device_id");
                return false;
            }
            out->op = info.disabled ? ActionOp::Unavailable : ActionOp::Adapter;
            out->adapterIndex = static_cast<uint8_t>(info.adapter);
            out->deviceIndex = static_cast<uint16_t>(device);
            catalog = info.catalog;
        }

        if (!resolveCommand(catalog, a.commandId, a.params, this, path, out, report_)) return false;
        if (out->command->effect == CommandEffect::Hold) {
            // The ledger releases it with this command (same params).
            const int release = out->command->releasedBy != nullptr ? catalog.find(out->command->releasedBy) : -1;
            if (release < 0) {
                report_.add(ErrorCode::HoldNotAllowed, path + "/command_id");  // nothing could release it
                return false;
            }
            out->release = &catalog.items[release];
            out->releaseIndex = static_cast<uint16_t>(release);
        }

        if (a.integrationId == kSystemIntegrationId) {
            switch (static_cast<SystemCommand>(out->commandIndex)) {
                case SystemCommand::Wait: out->op = ActionOp::Wait; break;
                case SystemCommand::Activate: out->op = ActionOp::Activate; break;
                case SystemCommand::Off: out->op = ActionOp::Off; break;
            }
        }
        return true;
    }

    const AdapterSet &adapters_;
    ValidationReport &report_;
    CompiledConfig *out_;
    const ConfigSpec *config_ = nullptr;
    std::vector<DeviceInfo> info_;
    std::vector<std::string> activityIds_;
    std::vector<CompiledAction> actions_;
};

}  // namespace

std::unique_ptr<CompiledConfig> compileConfig(const ConfigSpec &config, const AdapterSet &adapters,
                                              ValidationReport *report) {
    ValidationReport local;
    ValidationReport &r = report != nullptr ? *report : local;

    auto out = std::make_unique<CompiledConfig>();
    out->revision = config.revision;
    Compiler compiler(adapters, r, out.get());
    compiler.integrations(config.integrations, "/integrations");
    compiler.devices(config, "/devices");

    if (config.activities.size() > limits::kMaxActivities) {
        r.add(ErrorCode::TooManyActivities, "/activities");
        return nullptr;
    }
    std::vector<std::string> ids;
    ids.reserve(config.activities.size());
    for (const auto &a : config.activities) ids.push_back(a.id);
    compiler.activityIds(std::move(ids));

    out->activities.resize(config.activities.size());
    for (size_t i = 0; i < config.activities.size(); i++) {
        const ActivitySpec &a = config.activities[i];
        const std::string path = "/activities/" + std::to_string(i);
        if (isValidId(a.id) && indexOfId(config.activities, a.id, i) >= 0) {
            r.add(ErrorCode::DuplicateId, path + "/id");
        }
        compiler.activity(a, path, &out->activities[i]);
    }
    compiler.bindings(config.globalBindings, "/global_bindings", &out->globals);
    compiler.finish();

    if (!r.ok()) {
        return nullptr;
    }
    return out;
}

ValidationReport validateActivity(const ActivitySpec &activity, const ConfigSpec &context,
                                  const AdapterSet &adapters) {
    ValidationReport report;
    ValidationReport registry;  // registry problems aren't this activity's fault
    Compiler compiler(adapters, report, nullptr);
    compiler.devices(context, "/devices", &registry);

    // Indices don't matter for validation, only resolvability.
    std::vector<std::string> ids;
    bool replaces = false;
    for (const auto &a : context.activities) {
        ids.push_back(a.id);
        replaces = replaces || a.id == activity.id;
    }
    if (!replaces) {
        if (context.activities.size() >= limits::kMaxActivities) {
            report.add(ErrorCode::TooManyActivities, "");
        }
        ids.push_back(activity.id);
    }
    compiler.activityIds(std::move(ids));
    compiler.activity(activity, "", nullptr);
    compiler.finish();
    return report;
}

IntegrationResolution resolveIntegration(const ConfigSpec &config, const AdapterSet &adapters,
                                         const DeviceSpec &device) {
    IntegrationResolution r;
    const int index = indexOfId(config.integrations, device.integrationId);
    if (index < 0 || device.integrationId == kSystemIntegrationId) {
        r.error = ErrorCode::UnknownIntegration;
        return r;
    }
    r.integration = &config.integrations[static_cast<size_t>(index)];
    if (r.integration->type != device.provider.type) {
        r.error = ErrorCode::IntegrationMismatch;
        return r;
    }
    r.adapterIndex = adapters.find(device.integrationId);
    if (r.adapterIndex < 0) {
        r.error = ErrorCode::AdapterUnavailable;
        return r;
    }
    const char *adapterType = adapters.at(static_cast<size_t>(r.adapterIndex))->integrationType();
    if (adapterType != nullptr && r.integration->type != adapterType) {
        r.error = ErrorCode::IntegrationMismatch;
        return r;
    }
    if (!r.integration->enabled) r.error = ErrorCode::IntegrationDisabled;
    return r;
}

ErrorCode compileDeviceCommand(const ConfigSpec &config, const AdapterSet &adapters, std::string_view deviceId,
                               std::string_view commandId, const std::vector<ParamInput> &params,
                               CompiledCommand *out, ValidationReport *report) {
    ValidationReport local;
    ValidationReport &r = report != nullptr ? *report : local;
    const int device = indexOfId(config.devices, deviceId);
    if (device < 0) return ErrorCode::NotFound;
    const DeviceSpec &d = config.devices[static_cast<size_t>(device)];
    const IntegrationResolution resolved = resolveIntegration(config, adapters, d);
    if (resolved.error != ErrorCode::Ok) return resolved.error;

    const CommandCatalog catalog = adapters.at(static_cast<size_t>(resolved.adapterIndex))->commandsFor(d);
    CompiledCommand compiled;
    if (!resolveCommand(catalog, commandId, params, nullptr, "", &compiled.action, r)) {
        return catalog.find(commandId) < 0 ? ErrorCode::UnknownCommand : ErrorCode::ValidationFailed;
    }
    const ErrorCode misplaced = misplacedEffect(compiled.action.command, Place::Direct);
    if (misplaced != ErrorCode::Ok) {
        r.add(misplaced, "/command_id");
        return ErrorCode::ValidationFailed;
    }
    compiled.action.op = ActionOp::Adapter;
    compiled.action.adapterIndex = static_cast<uint8_t>(resolved.adapterIndex);
    compiled.action.deviceIndex = static_cast<uint16_t>(device);
    compiled.deviceId.assign(d.id);
    compiled.providerRef.assign(providerRef(d));
    *out = compiled;
    return ErrorCode::Ok;
}

}  // namespace hubbo
