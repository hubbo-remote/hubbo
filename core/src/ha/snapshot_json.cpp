#include "hubbo/ha/snapshot_json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string_view>

#include "hubbo/domain/limits.h"
#include "hubbo/ha/capabilities.h"

namespace hubbo::ha {

namespace {

constexpr size_t kMaxListItems = limits::kMaxEnumOptions;

const char *typeName(SnapshotParamType type) {
    switch (type) {
        case SnapshotParamType::Bool: return "bool";
        case SnapshotParamType::Int: return "int";
        case SnapshotParamType::Number: return "number";
        case SnapshotParamType::Enum: return "enum";
    }
    return "bool";
}

void putList(JsonObject out, const char *key, const std::vector<std::string> &items) {
    if (items.empty()) return;
    JsonArray list = out[key].to<JsonArray>();
    for (const auto &item : items) list.add(item);
}

bool isPlatform(std::string_view name) { return name.empty() || isHaName(name); }

// A tiny strict reader: every key must be one of `known`.
class Reader {
public:
    Reader(JsonVariantConst json, ValidationReport *report, std::string path)
        : report_(report), path_(std::move(path)) {
        if (!json.is<JsonObjectConst>()) {
            fail(ErrorCode::WrongType, path_);
        } else {
            object_ = json.as<JsonObjectConst>();
        }
    }

    bool ok() const { return ok_; }
    // `path`/`key`[/`index`].
    std::string at(std::string_view key, int index = -1) const {
        std::string path = path_;
        path += '/';
        path += key;
        if (index >= 0) {
            char digits[12];
            snprintf(digits, sizeof digits, "/%d", index);
            path += digits;
        }
        return path;
    }
    void fail(ErrorCode code, std::string path) {
        ok_ = false;
        report_->add(code, std::move(path));
    }
    void failAt(ErrorCode code, std::string_view key, int index = -1) {
        fail(code, at(key, index));
    }

    void taint() { ok_ = false; }  // a nested reader already reported why

    void only(std::initializer_list<const char *> known) {
        for (JsonPairConst pair : object_) {
            bool found = false;
            for (const char *k : known) found = found || std::string_view(pair.key().c_str()) == k;
            if (!found) failAt(ErrorCode::UnknownField, pair.key().c_str());
        }
    }

    JsonVariantConst get(const char *key, bool required) {
        if (object_.isNull()) return {};
        JsonVariantConst v = object_[key];
        if (v.isUnbound() && required) failAt(ErrorCode::Required, key);
        return v;
    }

    bool string(const char *key, std::string *out, size_t max, bool required = false) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        if (!v.is<const char *>()) return failAt(ErrorCode::WrongType, key), false;
        *out = v.as<const char *>();
        if (out->size() > max) return failAt(ErrorCode::TooLong, key), false;
        return true;
    }

    bool boolean(const char *key, bool *out, bool required = false) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        if (!v.is<bool>()) return failAt(ErrorCode::WrongType, key), false;
        *out = v.as<bool>();
        return true;
    }

    bool number(const char *key, double *out, bool required = false) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        if (!v.is<double>() || v.is<bool>() || !std::isfinite(v.as<double>())) {
            return failAt(ErrorCode::WrongType, key), false;
        }
        *out = v.as<double>();
        return true;
    }

    bool integer(const char *key, int64_t lo, int64_t hi, int64_t *out, bool required = false) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        if (!v.is<int64_t>() || v.is<bool>()) return failAt(ErrorCode::WrongType, key), false;
        *out = v.as<int64_t>();
        if (*out < lo || *out > hi) return failAt(ErrorCode::ParamOutOfRange, key), false;
        return true;
    }

    // A list of option strings (1..64 chars, at most kMaxListItems, no duplicates).
    bool options(const char *key, std::vector<std::string> *out, bool required = false) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        if (!v.is<JsonArrayConst>()) return failAt(ErrorCode::WrongType, key), false;
        JsonArrayConst list = v.as<JsonArrayConst>();
        if (list.size() > kMaxListItems) return failAt(ErrorCode::TooLong, key), false;
        int i = 0;
        bool good = true;
        for (JsonVariantConst item : list) {
            const int index = i++;
            if (!item.is<const char *>()) {
                failAt(ErrorCode::WrongType, key, index);
                good = false;
                continue;
            }
            std::string text = item.as<const char *>();
            if (text.empty() || text.size() > limits::kMaxParamStringLength) {
                failAt(ErrorCode::TooLong, key, index);
                good = false;
                continue;
            }
            for (const auto &seen : *out) {
                if (seen == text) {
                    failAt(ErrorCode::DuplicateId, key, index);
                    good = false;
                }
            }
            out->push_back(std::move(text));
        }
        return good;
    }

private:
    ValidationReport *report_;
    std::string path_;
    JsonObjectConst object_;
    bool ok_ = true;
};

void encodeParam(const SnapshotParam &p, JsonObject out) {
    out["name"] = p.name;
    if (!p.label.empty()) out["label"] = p.label;
    if (!p.description.empty()) out["description"] = p.description;
    out["type"] = typeName(p.type);
    out["required"] = p.required;
    if (p.type == SnapshotParamType::Int || p.type == SnapshotParamType::Number) {
        out["minimum"] = p.minimum;
        out["maximum"] = p.maximum;
        if (p.step > 0) out["step"] = p.step;
        if (!p.unit.empty()) out["unit"] = p.unit;
    }
    if (p.type == SnapshotParamType::Enum) putList(out, "options", p.options);
    if (!p.hasDefault) return;
    switch (p.type) {
        case SnapshotParamType::Bool: out["default"] = p.defaultNumber != 0; break;
        case SnapshotParamType::Int: out["default"] = static_cast<int64_t>(p.defaultNumber); break;
        case SnapshotParamType::Number: out["default"] = p.defaultNumber; break;
        case SnapshotParamType::Enum: out["default"] = p.defaultOption; break;
    }
}

// `default`, checked against the param's type, range and options.
bool decodeParam(JsonVariantConst json, SnapshotParam *out, ValidationReport *report, std::string path) {
    Reader r(json, report, std::move(path));
    if (!r.ok()) return false;
    r.only({"name", "label", "description", "type", "required", "minimum", "maximum", "step", "unit", "options",
            "default"});
    SnapshotParam &p = *out;  // a fresh param, decoded in place
    if (r.string("name", &p.name, 32, true) && (!isHaName(p.name) || isForbiddenParamName(p.name))) {
        r.failAt(ErrorCode::InvalidId, "name");
    }
    r.string("label", &p.label, kMaxSnapshotLabelLength);
    r.string("description", &p.description, kMaxSnapshotDescriptionLength);
    std::string type;
    if (r.string("type", &type, 16, true)) {
        if (type == "bool") {
            p.type = SnapshotParamType::Bool;
        } else if (type == "int") {
            p.type = SnapshotParamType::Int;
        } else if (type == "number") {
            p.type = SnapshotParamType::Number;
        } else if (type == "enum") {
            p.type = SnapshotParamType::Enum;
        } else {
            r.failAt(ErrorCode::WrongType, "type");
        }
    }
    r.boolean("required", &p.required, true);
    const bool ranged = p.type == SnapshotParamType::Int || p.type == SnapshotParamType::Number;
    if (ranged) {
        const bool lo = r.number("minimum", &p.minimum, true);
        const bool hi = r.number("maximum", &p.maximum, true);
        if (lo && hi && p.minimum > p.maximum) r.failAt(ErrorCode::ParamOutOfRange, "maximum");
        if (p.type == SnapshotParamType::Int && lo && hi &&
            (std::floor(p.minimum) != p.minimum || std::floor(p.maximum) != p.maximum ||
             p.minimum < -2147483648.0 || p.maximum > 2147483647.0)) {
            r.failAt(ErrorCode::WrongType, "minimum");
        }
        if (r.number("step", &p.step) && !(p.step > 0)) r.failAt(ErrorCode::ParamOutOfRange, "step");
        r.string("unit", &p.unit, kMaxSnapshotUnitLength);
    } else {
        for (const char *key : {"minimum", "maximum", "step", "unit"}) {
            if (!r.get(key, false).isUnbound()) r.failAt(ErrorCode::UnknownField, key);
        }
    }
    if (p.type == SnapshotParamType::Enum) {
        if (r.options("options", &p.options, true) && p.options.empty()) r.failAt(ErrorCode::Required, "options");
    } else if (!r.get("options", false).isUnbound()) {
        r.failAt(ErrorCode::UnknownField, "options");
    }
    if (r.ok()) {
        const JsonVariantConst value = r.get("default", false);
        if (!value.isUnbound() && !value.isNull() && !applyDefault(value, &p)) {
            r.failAt(ErrorCode::ParamOutOfRange, "default");
        }
    }
    return r.ok();
}

}  // namespace

bool applyDefault(JsonVariantConst v, SnapshotParam *param) {
    switch (param->type) {
        case SnapshotParamType::Bool:
            if (!v.is<bool>()) return false;
            param->defaultNumber = v.as<bool>() ? 1 : 0;
            break;
        case SnapshotParamType::Int:
        case SnapshotParamType::Number: {
            if (!v.is<double>() || v.is<bool>()) return false;
            const double d = v.as<double>();
            if (!(d >= param->minimum && d <= param->maximum)) return false;
            if (param->type == SnapshotParamType::Int && std::floor(d) != d) return false;
            param->defaultNumber = d;
            break;
        }
        case SnapshotParamType::Enum: {
            if (!v.is<const char *>()) return false;
            const std::string option = v.as<const char *>();
            if (std::find(param->options.begin(), param->options.end(), option) == param->options.end()) return false;
            param->defaultOption = option;
            break;
        }
    }
    param->hasDefault = true;
    return true;
}

void encodeSnapshot(const CapabilitySnapshot &s, JsonObject out) {
    out["platform"] = s.platform;
    out["supported_features"] = s.supportedFeatures;
    putList(out, "source_list", s.sourceList);
    putList(out, "sound_mode_list", s.soundModeList);
    putList(out, "supported_color_modes", s.supportedColorModes);
    if (s.minColorTempKelvin > 0) out["min_color_temp_kelvin"] = s.minColorTempKelvin;
    if (s.maxColorTempKelvin > 0) out["max_color_temp_kelvin"] = s.maxColorTempKelvin;
    putList(out, "preset_modes", s.presetModes);
    if (s.percentageStep > 0) out["percentage_step"] = s.percentageStep;
    if (s.minHumidity < s.maxHumidity) {
        out["min_humidity"] = s.minHumidity;
        out["max_humidity"] = s.maxHumidity;
    }
    putList(out, "available_modes", s.availableModes);
    putList(out, "options", s.options);
    JsonArray services = out["services"].to<JsonArray>();
    for (const auto &service : s.services) {
        JsonObject o = services.add<JsonObject>();
        o["service"] = service.service;
        if (!service.label.empty()) o["label"] = service.label;
        JsonArray fields = o["fields"].to<JsonArray>();
        for (const auto &field : service.fields) encodeParam(field, fields.add<JsonObject>());
    }
    if (!s.customServices.empty()) encodeCustomServices(s.customServices, out["custom_services"].to<JsonArray>());
}

void encodeCustomServices(const std::vector<SnapshotCustomService> &services, JsonArray out) {
    for (const auto &service : services) {
        JsonObject o = out.add<JsonObject>();
        o["service"] = service.service;
        if (!service.label.empty()) o["label"] = service.label;
        JsonArray fields = o["fields"].to<JsonArray>();
        for (const auto &hint : service.fields) {
            JsonObject f = fields.add<JsonObject>();
            f["name"] = hint.name;
            f["required"] = hint.required;
            if (!hint.example.empty()) {
                JsonDocument example;  // a copy, so the result reads back as JSON, not raw text
                if (!deserializeJson(example, hint.example)) f["example"] = example.as<JsonVariantConst>();
            }
            if (!hint.description.empty()) f["description"] = hint.description;
        }
    }
}

namespace {

// "<platform>.<service>" of the snapshot's own platform.
void checkServiceId(Reader &r, const std::string &id, const std::string &platform) {
    // Only the platform's own services (capabilities.cpp re-checks).
    if (!isHaServiceId(id) || id.substr(0, id.find('.')) != platform) r.failAt(ErrorCode::InvalidCommandId, "service");
}

// One entry of `services`, decoded in place onto the end of `services` (and
// dropped again on false).
bool decodeService(JsonVariantConst item, const std::string &platform, std::vector<SnapshotService> *services,
                   ValidationReport *report, std::string where) {
    Reader sr(item, report, std::move(where));
    if (!sr.ok()) return false;
    sr.only({"service", "label", "fields"});
    SnapshotService &service = services->emplace_back();
    if (sr.string("service", &service.service, limits::kMaxCommandIdLength, true)) {
        checkServiceId(sr, service.service, platform);
    }
    sr.string("label", &service.label, kMaxSnapshotLabelLength);
    JsonVariantConst fields = sr.get("fields", true);
    if (!fields.isUnbound()) {
        if (!fields.is<JsonArrayConst>()) {
            sr.failAt(ErrorCode::WrongType, "fields");
        } else if (fields.size() > limits::kMaxParams) {
            sr.failAt(ErrorCode::TooManyParams, "fields");
        } else {
            int f = 0;
            for (JsonVariantConst field : fields.as<JsonArrayConst>()) {
                const std::string fieldPath = sr.at("fields", f++);
                if (!decodeParam(field, &service.fields.emplace_back(), report, fieldPath)) {
                    service.fields.pop_back();
                    sr.taint();
                    continue;
                }
                const std::string &name = service.fields.back().name;
                for (size_t k = 0; k + 1 < service.fields.size(); k++) {
                    if (service.fields[k].name == name) sr.fail(ErrorCode::DuplicateParam, fieldPath + "/name");
                }
            }
        }
    }
    if (sr.ok()) return true;
    services->pop_back();
    return false;
}

bool decodeHint(JsonVariantConst item, std::vector<SnapshotHint> *hints, ValidationReport *report, std::string where) {
    Reader r(item, report, std::move(where));
    if (!r.ok()) return false;
    r.only({"name", "required", "example", "description"});
    SnapshotHint &hint = hints->emplace_back();
    if (r.string("name", &hint.name, 32, true) && (!isHaName(hint.name) || isForbiddenParamName(hint.name))) {
        r.failAt(ErrorCode::InvalidId, "name");
    }
    r.boolean("required", &hint.required, true);
    const JsonVariantConst example = r.get("example", false);
    if (!example.isUnbound()) {
        serializeJson(example, hint.example);
        if (example.isNull() || hint.example.size() > kMaxHintExampleBytes) r.failAt(ErrorCode::TooLong, "example");
    }
    r.string("description", &hint.description, kMaxSnapshotDescriptionLength);
    for (size_t k = 0; k + 1 < hints->size(); k++) {
        if ((*hints)[k].name == hint.name) r.failAt(ErrorCode::DuplicateParam, "name");
    }
    if (r.ok()) return true;
    hints->pop_back();
    return false;
}

// A custom service: the platform's own, or on a remote HA's `remote` ones.
bool decodeCustomService(JsonVariantConst item, const std::string &platform, std::string_view entityId,
                         std::vector<SnapshotCustomService> *services, ValidationReport *report, std::string where) {
    Reader r(item, report, std::move(where));
    if (!r.ok()) return false;
    r.only({"service", "label", "fields"});
    SnapshotCustomService &service = services->emplace_back();
    if (r.string("service", &service.service, limits::kMaxCommandIdLength, true) &&
        !isDeviceService(entityId, platform, service.service)) {
        r.failAt(ErrorCode::InvalidCommandId, "service");
    }
    for (size_t k = 0; k + 1 < services->size(); k++) {
        if ((*services)[k].service == service.service) r.failAt(ErrorCode::DuplicateId, "service");
    }
    r.string("label", &service.label, kMaxSnapshotLabelLength);
    JsonVariantConst fields = r.get("fields", true);
    if (!fields.isUnbound()) {
        if (!fields.is<JsonArrayConst>()) {
            r.failAt(ErrorCode::WrongType, "fields");
        } else if (fields.size() > kMaxCustomFields) {
            r.failAt(ErrorCode::TooLong, "fields");
        } else {
            int f = 0;
            for (JsonVariantConst field : fields.as<JsonArrayConst>()) {
                if (!decodeHint(field, &service.fields, report, r.at("fields", f++))) r.taint();
            }
        }
    }
    if (r.ok()) return true;
    services->pop_back();
    return false;
}

}  // namespace

bool decodeSnapshot(JsonVariantConst json, CapabilitySnapshot *out, ValidationReport *report,
                    const std::string &path, std::string_view entityId) {
    Reader r(json, report, path);
    if (!r.ok()) return false;
    r.only({"platform", "supported_features", "source_list", "sound_mode_list", "supported_color_modes",
            "min_color_temp_kelvin", "max_color_temp_kelvin", "preset_modes", "percentage_step", "min_humidity",
            "max_humidity", "available_modes", "options", "services", "custom_services"});
    CapabilitySnapshot &s = *out;  // decoded in place: no snapshot on this frame
    if (r.string("platform", &s.platform, 32, true) && !isPlatform(s.platform)) {
        r.failAt(ErrorCode::InvalidId, "platform");
    }
    int64_t n = 0;
    if (r.integer("supported_features", 0, 0xFFFFFFFFLL, &n, true)) s.supportedFeatures = static_cast<uint32_t>(n);
    r.options("source_list", &s.sourceList);
    r.options("sound_mode_list", &s.soundModeList);
    r.options("supported_color_modes", &s.supportedColorModes);
    if (r.integer("min_color_temp_kelvin", 0, 100000, &n)) s.minColorTempKelvin = static_cast<int32_t>(n);
    if (r.integer("max_color_temp_kelvin", 0, 100000, &n)) s.maxColorTempKelvin = static_cast<int32_t>(n);
    r.options("preset_modes", &s.presetModes);
    if (r.integer("percentage_step", 1, 100, &n)) s.percentageStep = static_cast<int32_t>(n);
    // Both or neither (makeSnapshot writes humidityRange()'s result), min < max.
    const bool hasMin = r.integer("min_humidity", 0, 100, &n);
    if (hasMin) s.minHumidity = static_cast<int32_t>(n);
    const bool hasMax = r.integer("max_humidity", 0, 100, &n);
    if (hasMax) s.maxHumidity = static_cast<int32_t>(n);
    if (hasMin && !hasMax) r.failAt(ErrorCode::Required, "max_humidity");
    if (hasMax && !hasMin) r.failAt(ErrorCode::Required, "min_humidity");
    if (hasMin && hasMax && s.minHumidity >= s.maxHumidity) r.failAt(ErrorCode::ParamOutOfRange, "max_humidity");
    r.options("available_modes", &s.availableModes);
    r.options("options", &s.options);

    JsonVariantConst services = r.get("services", true);
    if (!services.isUnbound()) {
        if (!services.is<JsonArrayConst>()) {
            r.failAt(ErrorCode::WrongType, "services");
        } else if (services.size() > kMaxSnapshotServices) {
            r.failAt(ErrorCode::TooLong, "services");
        } else {
            int i = 0;
            for (JsonVariantConst item : services.as<JsonArrayConst>()) {
                if (!decodeService(item, s.platform, &s.services, report, r.at("services", i++))) {
                    r.taint();
                }
            }
        }
    }
    JsonVariantConst custom = r.get("custom_services", false);
    if (!custom.isUnbound()) {
        if (!offersIntegrationServices(entityId)) {
            r.failAt(ErrorCode::UnknownField, "custom_services");  // never offered (a scene, ...)
        } else if (!custom.is<JsonArrayConst>()) {
            r.failAt(ErrorCode::WrongType, "custom_services");
        } else if (custom.size() > kMaxCustomServices) {
            r.failAt(ErrorCode::TooLong, "custom_services");
        } else {
            int i = 0;
            for (JsonVariantConst item : custom.as<JsonArrayConst>()) {
                if (!decodeCustomService(item, s.platform, entityId, &s.customServices, report,
                                         r.at("custom_services", i++))) {
                    r.taint();
                }
            }
            if (customHintBytes(s) > kMaxCustomHintBytes) r.failAt(ErrorCode::TooLong, "custom_services");
        }
    }
    return r.ok();
}

}  // namespace hubbo::ha
