#include "fakes.h"

#include <algorithm>
#include <cstdio>
#include <utility>
#include <iterator>

#include "fixtures.h"
#include "hubbo/domain/time.h"

namespace hubbo::sim {

namespace {

constexpr ParamSpec kIrCode[] = {ParamSpec::integer("code", 0, 0xFFFF)};
constexpr CommandSpec kIr[] = {
    {"power_on", nullptr, 0},
    {"power_off", nullptr, 0},
    {"send", kIrCode, 1},
};

}  // namespace

std::string formatNumber(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

std::string quoteIfSpaced(const std::string &s) {
    return s.find(' ') == std::string::npos ? s : "\"" + s + "\"";
}

std::string formatParams(const CommandSpec &command, const ParamValue *params, uint8_t present) {
    std::string out;
    for (uint8_t i = 0; i < command.paramCount; i++) {
        const ParamSpec &spec = command.params[i];
        const ParamValue &v = params[i];
        if (!spec.required && (present & (1u << i)) == 0) continue;
        if (!out.empty()) out += ' ';
        out += spec.name;
        out += '=';
        switch (spec.type) {
            case ParamType::Bool: out += v.asBool() ? "true" : "false"; break;
            case ParamType::Int:
            case ParamType::ActivityRef: out += std::to_string(v.integer); break;
            case ParamType::Number: out += formatNumber(v.number); break;
            case ParamType::Enum:
                out += v.asOption() < spec.optionCount ? quoteIfSpaced(spec.options[v.asOption()]) : "?";
                break;
        }
    }
    return out;
}

integrations::BleHostStatus SimulatedBleHosts::status(std::string_view deviceId) const {
    integrations::BleHostStatus s;
    if (const Host *h = host(deviceId)) {
        s.bonded = true;
        s.connected = h->connected;
    }
    s.pairing = windowDevice_ == deviceId;
    s.windowRemainingMs = s.pairing ? windowRemainingMs_ : 0;
    return s;
}

integrations::BleHostResult SimulatedBleHosts::startPairing(std::string_view deviceId, uint32_t windowMs) {
    if (!ready) return integrations::BleHostResult::NotReady;
    if (windowDevice_ == deviceId) return integrations::BleHostResult::Accepted;  // already open
    if (!windowDevice_.empty()) return integrations::BleHostResult::Busy;
    windowDevice_ = deviceId;  // the old bond stays until a new one replaces it
    windowRemainingMs_ = windowMs;
    return integrations::BleHostResult::Accepted;
}

integrations::BleHostResult SimulatedBleHosts::cancelPairing(std::string_view deviceId) {
    if (!ready) return integrations::BleHostResult::NotReady;
    if (windowDevice_ == deviceId) windowDevice_.clear();
    return integrations::BleHostResult::Accepted;
}

integrations::BleHostResult SimulatedBleHosts::forget(std::string_view deviceId) {
    if (!ready) return integrations::BleHostResult::NotReady;
    if (windowDevice_ == deviceId) windowDevice_.clear();
    eraseHosts([&](const Host &h) { return h.deviceId == deviceId; });
    return integrations::BleHostResult::Accepted;
}

void SimulatedBleHosts::retainOnly(const integrations::BleDeviceIds &devices) {
    if (!devices.contains(windowDevice_)) windowDevice_.clear();
    eraseHosts([&](const Host &h) { return !devices.contains(h.deviceId); });
}

template <typename Pred>
void SimulatedBleHosts::eraseHosts(Pred gone) {
    const size_t before = hosts_.size();
    hosts_.erase(std::remove_if(hosts_.begin(), hosts_.end(), gone), hosts_.end());
    if (hosts_.size() != before) bondsChanged();
}

void SimulatedBleHosts::setHost(const std::string &deviceId, const std::string &hostName) {
    if (Host *own = host(deviceId)) {
        *own = Host{deviceId, hostName, true};  // replaces only its own bond
    } else {
        hosts_.push_back(Host{deviceId, hostName, true});
    }
    bondsChanged();
}

bool SimulatedBleHosts::connected(std::string_view deviceId) const {
    const Host *h = host(deviceId);
    return ready && h != nullptr && h->connected;
}

bool SimulatedBleHosts::sendKeyboard(std::string_view deviceId, const uint8_t report[8]) {
    if (!connected(deviceId)) return false;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "keyboard %02x %02x %02x %02x %02x %02x %02x %02x", report[0], report[1],
                  report[2], report[3], report[4], report[5], report[6], report[7]);
    record(deviceId, buf);
    return true;
}

bool SimulatedBleHosts::sendConsumer(std::string_view deviceId, uint16_t usage) {
    if (!connected(deviceId)) return false;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "consumer 0x%04x", usage);
    record(deviceId, buf);
    return true;
}

void SimulatedBleHosts::advance(uint32_t elapsedMs) {
    if (windowDevice_.empty()) return;
    if (elapsedMs < windowRemainingMs_) {
        windowRemainingMs_ -= elapsedMs;
        return;
    }
    windowDevice_.clear();  // closed with no new bond; the old one (if any) stays
}

bool SimulatedBleHosts::bonded(const std::string &hostName) {
    if (windowDevice_.empty()) return false;
    for (const Host &h : hosts_) {
        if (h.name == hostName && h.deviceId != windowDevice_) return false;  // tied to its own device
    }
    const std::string device = std::exchange(windowDevice_, std::string());
    if (host(device) == nullptr && hosts_.size() >= limits::kMaxBleDevices) return false;  // the store is full
    setHost(device, hostName);
    return true;
}

void SimulatedBleHosts::bond(const std::string &deviceId, const std::string &hostName) { setHost(deviceId, hostName); }

void SimulatedBleHosts::reset() {
    hosts_.clear();
    windowDevice_.clear();
    windowRemainingMs_ = 0;
    ready = true;
    bondsChanged();
}

std::string SimulatedBleHosts::saveBonds() const {
    std::string text;
    for (const Host &h : hosts_) text.append(h.deviceId).append(1, '\t').append(h.name).append(1, '\n');
    return text;
}

bool SimulatedBleHosts::loadBonds(const std::string &text) {
    std::vector<Host> loaded;
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find('\n', start);
        if (end == std::string::npos) return false;
        const std::string line = text.substr(start, end - start);
        const size_t tab = line.find('\t');
        if (tab == 0 || tab == std::string::npos || loaded.size() == limits::kMaxBleDevices) return false;
        loaded.push_back(Host{line.substr(0, tab), line.substr(tab + 1), true});
        start = end + 1;
    }
    hosts_ = std::move(loaded);
    return true;
}

void SimulatedBleHosts::bondsChanged() {
    if (onBondsChanged) onBondsChanged();
}

bool SimulatedBleHosts::setConnected(std::string_view deviceId, bool connected) {
    Host *h = host(deviceId);
    if (h == nullptr) return false;
    h->connected = connected;
    return true;
}

void SimulatedBleHosts::setAllConnected(bool connected) {
    for (Host &h : hosts_) h.connected = connected;
}

SimulatedBleHosts::Host *SimulatedBleHosts::host(std::string_view deviceId) {
    for (Host &h : hosts_) {
        if (h.deviceId == deviceId) return &h;
    }
    return nullptr;
}

const SimulatedBleHosts::Host *SimulatedBleHosts::host(std::string_view deviceId) const {
    return const_cast<SimulatedBleHosts *>(this)->host(deviceId);
}

void SimulatedBleHosts::record(std::string_view deviceId, std::string report) {
    reports_.push_back(std::move(report));
    reportDevices_.emplace_back(deviceId);
}

void FakeHomeAssistant::addEntity(const ha::EntityInfo &entity) {
    entities_[entity.entityId] = &catalogs_.keep(ha::EntityCapabilities::fromEntity(entity));
}

CommandCatalog FakeHomeAssistant::commandsFor(const DeviceSpec &device) const {
    if (device.haCapabilities) return catalogs_.commandsFor(device);
    const auto it = entities_.find(providerRef(device));
    return it == entities_.end() ? CommandCatalog{} : it->second->catalog();
}

void FakeHomeAssistant::failNext(std::string commandId, ErrorCode error) {
    failures_.push_back({std::move(commandId), error});
}

ExecResult FakeHomeAssistant::execute(const ExecRequest &request) {
    if (!available_) return ExecResult::failed(ErrorCode::AdapterUnavailable);

    // A fake: it may lock on the engine task (the real adapter doesn't).
    const std::string entityId(request.providerRef);
    const ha::EntityCapabilities *caps = catalogs_.owner(request.command, entityId);
    if (caps == nullptr) return ExecResult::failed(ErrorCode::AdapterFailed);
    const char *stateKey = request.call ? nullptr : caps->stateAttribute(request.commandIndex);
    const ha::ServiceValue *state = stateKey != nullptr ? attribute(entityId, stateKey) : nullptr;
    ha::ServiceCall call;
    const ErrorCode built = integrations::HaCatalogs::buildCall(*caps, request.commandIndex, request.params,
                                                                request.paramsPresent, request.call.get(), state, &call);
    if (built != ErrorCode::Ok) return ExecResult::failed(built);

    std::string line = call.domain + "." + call.service + " " + call.entityId;
    if (!call.rawData.empty()) line += " " + call.rawData;
    for (const auto &field : call.data) {
        line += " " + field.key + "=";
        switch (field.value.kind) {
            case ha::ServiceValue::Kind::Bool: line += field.value.boolean ? "true" : "false"; break;
            case ha::ServiceValue::Kind::Int: line += std::to_string(field.value.integer); break;
            case ha::ServiceValue::Kind::Number: line += formatNumber(field.value.number); break;
            case ha::ServiceValue::Kind::String: line += quoteIfSpaced(field.value.string); break;
        }
    }
    calls_.push_back(std::move(line));

    ExecResult result = ExecResult::accepted();
    for (auto it = failures_.begin(); it != failures_.end(); ++it) {
        if (it->commandId == request.command->id) {
            result = it->error == ErrorCode::AdapterTimeout ? ExecResult::timeout() : ExecResult::failed(it->error);
            failures_.erase(it);
            break;
        }
    }
    if (result.status == ExecStatus::Accepted) {  // HA applied it
        for (const auto &field : call.data) state_[call.entityId][field.key] = field.value;
    }
    if (latencyMs_ == 0) return result;
    delayed_.push_back({request.ticket, request.nowMs + latencyMs_, result});
    return ExecResult::pending();
}

const ha::ServiceValue *FakeHomeAssistant::attribute(const std::string &entityId, const std::string &key) const {
    const auto entity = state_.find(entityId);
    if (entity == state_.end()) return nullptr;
    const auto value = entity->second.find(key);
    return value == entity->second.end() ? nullptr : &value->second;
}

void FakeHomeAssistant::poll(uint32_t nowMs, CompletionSink &sink) {
    while (!delayed_.empty() && timeReached(nowMs, delayed_.front().dueMs)) {
        const Delayed d = delayed_.front();
        delayed_.pop_front();
        sink.complete(d.ticket, d.result, nowMs);
    }
}

CommandCatalog FakeIr::commandsFor(const DeviceSpec &device) const {
    (void)device;
    return {kIr, static_cast<uint16_t>(std::size(kIr))};
}

ExecResult FakeIr::execute(const ExecRequest &request) {
    (void)request;
    return ExecResult::failed(ErrorCode::NotImplemented);
}

FakeAdapters::FakeAdapters(IntegrationAdapter *realHomeAssistant) {
    for (const auto &entity : fixtureEntities()) homeAssistant.addEntity(entity);
    set.add(ble);
    if (realHomeAssistant != nullptr) {
        set.add(*realHomeAssistant);
    } else {
        set.add(homeAssistant);
    }
    set.add(ir);
}

}  // namespace hubbo::sim
