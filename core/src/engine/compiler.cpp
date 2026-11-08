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

const std::string &idOf(const std::string &id) { return id; }
template <typename T>
const std::string &idOf(const T &item) {
    return item.id;
}

// Index of the first of items[0, end) with this id, or -1.
template <typename T>
int indexOfId(const std::vector<T> &items, std::string_view id, size_t end = SIZE_MAX) {
    end = std::min(end, items.size());
    for (size_t i = 0; i < end; i++) {
        if (idOf(items[i]) == id) return static_cast<int>(i);
    }
    return -1;
}

struct DeviceInfo {
    bool valid = false;
    int adapter = -1;
    CommandCatalog catalog;
};

class Compiler : public RefResolver {
public:
    Compiler(const AdapterSet &adapters, ValidationReport &report, CompiledConfig *out)
        : adapters_(adapters), report_(report), out_(out) {}

    // Devices first: actions resolve against them. Device problems go to
    // `sink` (defaults to the main report).
    void devices(const std::vector<DeviceSpec> &devices, const std::string &prefix,
                 ValidationReport *sink = nullptr) {
        ValidationReport &report = sink != nullptr ? *sink : report_;
        devices_ = &devices;
        info_.assign(devices.size(), DeviceInfo{});
        if (devices.size() > limits::kMaxDevices) {
            report.add(ErrorCode::TooManyDevices, prefix);
            return;
        }
        for (size_t i = 0; i < devices.size(); i++) {
            const DeviceSpec &d = devices[i];
            const std::string path = prefix + "/" + std::to_string(i);
            bool ok = true;
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
            const int adapter = isValidId(d.integrationId) ? adapters_.find(d.integrationId) : -1;
            if (adapter < 0) {
                report.add(isValidId(d.integrationId) ? ErrorCode::UnknownIntegration
                                                       : ErrorCode::InvalidId,
                            path + "/integration_id");
                ok = false;
            }
            if (d.providerRef.empty() || d.providerRef.size() > limits::kMaxProviderRefLength) {
                report.add(ErrorCode::InvalidId, path + "/provider_ref");
                ok = false;
            }
            if (!ok) continue;
            info_[i].valid = true;
            info_[i].adapter = adapter;
            info_[i].catalog = adapters_.at(static_cast<size_t>(adapter))->commandsFor(d);
        }
        if (out_ != nullptr) {
            out_->devices.resize(devices.size());
            for (size_t i = 0; i < devices.size(); i++) {
                out_->devices[i].id.assign(devices[i].id);
                out_->devices[i].providerRef.assign(devices[i].providerRef);
            }
        }
    }

    // The ids system.activate may reference, index = compiled activity index.
    void activityIds(std::vector<std::string> ids) { activityIds_ = std::move(ids); }

    int activityIndex(std::string_view id) const override { return indexOfId(activityIds_, id); }

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

    void bindings(const std::vector<BindingSpec> &bindings, const std::string &prefix, TriggerTable *table) {
        bool seen[kTriggerCount] = {};
        for (const auto &binding : bindings) {
            const std::string path = prefix + "/" + jsonPointerToken(binding.trigger);
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
            ActionSpan span;
            sequence(binding.actions, /*isBinding=*/true, path, &span);
            table->spans[static_cast<size_t>(trigger.button)][static_cast<size_t>(trigger.kind)] = span;
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
    // system.activate/off are allowed only as the last action of a binding.
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
            const int adapter = adapters_.find(a.integrationId);
            if (adapter < 0) {
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
            const int device = indexOfId(*devices_, *a.deviceId);
            if (device < 0 || !info_[static_cast<size_t>(device)].valid) {
                report_.add(ErrorCode::UnknownDevice, path + "/device_id");
                return false;
            }
            const DeviceInfo &info = info_[static_cast<size_t>(device)];
            if (info.adapter != adapter) {
                report_.add(ErrorCode::IntegrationMismatch, path + "/device_id");
                return false;
            }
            out->op = ActionOp::Adapter;
            out->adapterIndex = static_cast<uint8_t>(adapter);
            out->deviceIndex = static_cast<uint16_t>(device);
            catalog = info.catalog;
        }

        const int command = catalog.find(a.commandId);
        if (command < 0) {
            report_.add(ErrorCode::UnknownCommand, path + "/command_id");
            return false;
        }
        const CommandSpec &spec = catalog.items[command];
        std::string badParam;
        const ErrorCode code = resolveParams(spec, a.params, out->params, &badParam, this);
        if (code != ErrorCode::Ok) {
            report_.add(code, badParam.empty() ? path + "/params"
                                               : path + "/params/" + jsonPointerToken(badParam));
            return false;
        }
        out->command = &spec;
        out->commandIndex = static_cast<uint16_t>(command);

        if (a.integrationId == kSystemIntegrationId) {
            switch (static_cast<SystemCommand>(command)) {
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
    const std::vector<DeviceSpec> *devices_ = nullptr;
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
    compiler.devices(config.devices, "/devices");

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
    compiler.devices(context.devices, "/devices", &registry);

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

}  // namespace hubbo
