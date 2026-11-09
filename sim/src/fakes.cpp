#include "fakes.h"

#include <cstdio>
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

bool RecordingHidTransport::sendKeyboard(const uint8_t report[8]) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "keyboard %02x %02x %02x %02x %02x %02x %02x %02x", report[0], report[1],
                  report[2], report[3], report[4], report[5], report[6], report[7]);
    reports_.emplace_back(buf);
    return true;
}

bool RecordingHidTransport::sendConsumer(uint16_t usage) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "consumer 0x%04x", usage);
    reports_.emplace_back(buf);
    return true;
}

void FakeHomeAssistant::addEntity(const ha::EntityInfo &entity) {
    entities_.erase(entity.entityId);
    entities_.emplace(entity.entityId, ha::EntityCapabilities::fromEntity(entity));
}

CommandCatalog FakeHomeAssistant::commandsFor(const DeviceSpec &device) const {
    const auto it = entities_.find(providerRef(device));
    return it == entities_.end() ? CommandCatalog{} : it->second.catalog();
}

void FakeHomeAssistant::failNext(std::string commandId, ErrorCode error) {
    failures_.push_back({std::move(commandId), error});
}

ExecResult FakeHomeAssistant::execute(const ExecRequest &request) {
    if (!available_) return ExecResult::failed(ErrorCode::AdapterUnavailable);

    const auto entity = entities_.find(request.providerRef);
    if (entity == entities_.end()) return ExecResult::failed(ErrorCode::AdapterFailed);
    ha::ServiceCall call;
    const ErrorCode built = entity->second.buildServiceCall(request.commandIndex, request.params, request.paramsPresent, &call);
    if (built != ErrorCode::Ok) return ExecResult::failed(built);

    std::string line = call.domain + "." + call.service + " " + call.entityId;
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
    if (latencyMs_ == 0) return result;
    delayed_.push_back({request.ticket, request.nowMs + latencyMs_, result});
    return ExecResult::pending();
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
