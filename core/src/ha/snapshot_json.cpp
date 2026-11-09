#include "hubbo/ha/snapshot_json.h"

#include <cmath>
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
    std::string at(std::string_view key) const { return path_ + "/" + std::string(key); }
    void fail(ErrorCode code, std::string path) {
        ok_ = false;
        report_->add(code, std::move(path));
    }

    void taint() { ok_ = false; }  // a nested reader already reported why

    void only(std::initializer_list<const char *> known) {
        for (JsonPairConst pair : object_) {
            bool found = false;
            for (const char *k : known) found = found || std::string_view(pair.key().c_str()) == k;
            if (!found) fail(ErrorCode::UnknownField, at(pair.key().c_str()));
        }
    }

    JsonVariantConst get(const char *key, bool required) {
        if (object_.isNull()) return {};
        JsonVariantConst v = object_[key];
        if (v.isUnbound() && required) fail(ErrorCode::Required, at(key));
        return v;
    }

    bool string(const char *key, std::string *out, size_t max, bool required = false) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        if (!v.is<const char *>()) return fail(ErrorCode::WrongType, at(key)), false;
        *out = v.as<const char *>();
        if (out->size() > max) return fail(ErrorCode::TooLong, at(key)), false;
        return true;
    }

    bool boolean(const char *key, bool *out, bool required = false) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        if (!v.is<bool>()) return fail(ErrorCode::WrongType, at(key)), false;
        *out = v.as<bool>();
        return true;
    }

    bool number(const char *key, double *out, bool required = false) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        if (!v.is<double>() || v.is<bool>() || !std::isfinite(v.as<double>())) {
            return fail(ErrorCode::WrongType, at(key)), false;
        }
        *out = v.as<double>();
        return true;
    }

    bool integer(const char *key, int64_t lo, int64_t hi, int64_t *out, bool required = false) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        if (!v.is<int64_t>() || v.is<bool>()) return fail(ErrorCode::WrongType, at(key)), false;
        *out = v.as<int64_t>();
        if (*out < lo || *out > hi) return fail(ErrorCode::ParamOutOfRange, at(key)), false;
        return true;
    }

    // A list of option strings (1..64 chars, at most kMaxListItems, no duplicates).
    bool options(const char *key, std::vector<std::string> *out, bool required = false) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        if (!v.is<JsonArrayConst>()) return fail(ErrorCode::WrongType, at(key)), false;
        JsonArrayConst list = v.as<JsonArrayConst>();
        if (list.size() > kMaxListItems) return fail(ErrorCode::TooLong, at(key)), false;
        size_t i = 0;
        bool good = true;
        for (JsonVariantConst item : list) {
            const std::string where = at(key) + "/" + std::to_string(i++);
            if (!item.is<const char *>()) {
                fail(ErrorCode::WrongType, where);
                good = false;
                continue;
            }
            std::string text = item.as<const char *>();
            if (text.empty() || text.size() > limits::kMaxParamStringLength) {
                fail(ErrorCode::TooLong, where);
                good = false;
                continue;
            }
            for (const auto &seen : *out) {
                if (seen == text) {
                    fail(ErrorCode::DuplicateId, where);
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
    out["type"] = typeName(p.type);
    out["required"] = p.required;
    if (p.type == SnapshotParamType::Int || p.type == SnapshotParamType::Number) {
        out["minimum"] = p.minimum;
        out["maximum"] = p.maximum;
        if (p.step > 0) out["step"] = p.step;
        if (!p.unit.empty()) out["unit"] = p.unit;
    }
    if (p.type == SnapshotParamType::Enum) putList(out, "options", p.options);
}

bool decodeParam(JsonVariantConst json, SnapshotParam *out, ValidationReport *report, const std::string &path) {
    Reader r(json, report, path);
    if (!r.ok()) return false;
    r.only({"name", "label", "type", "required", "minimum", "maximum", "step", "unit", "options", "default"});
    SnapshotParam p;
    if (r.string("name", &p.name, 32, true) && (!isHaName(p.name) || isForbiddenParamName(p.name))) {
        r.fail(ErrorCode::InvalidId, r.at("name"));
    }
    r.string("label", &p.label, kMaxSnapshotLabelLength);
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
            r.fail(ErrorCode::WrongType, r.at("type"));
        }
    }
    r.boolean("required", &p.required, true);
    const bool ranged = p.type == SnapshotParamType::Int || p.type == SnapshotParamType::Number;
    if (ranged) {
        const bool lo = r.number("minimum", &p.minimum, true);
        const bool hi = r.number("maximum", &p.maximum, true);
        if (lo && hi && p.minimum > p.maximum) r.fail(ErrorCode::ParamOutOfRange, r.at("maximum"));
        if (p.type == SnapshotParamType::Int && lo && hi &&
            (std::floor(p.minimum) != p.minimum || std::floor(p.maximum) != p.maximum ||
             p.minimum < -2147483648.0 || p.maximum > 2147483647.0)) {
            r.fail(ErrorCode::WrongType, r.at("minimum"));
        }
        if (r.number("step", &p.step) && !(p.step > 0)) r.fail(ErrorCode::ParamOutOfRange, r.at("step"));
        r.string("unit", &p.unit, kMaxSnapshotUnitLength);
    } else {
        for (const char *key : {"minimum", "maximum", "step", "unit"}) {
            if (!r.get(key, false).isUnbound()) r.fail(ErrorCode::UnknownField, r.at(key));
        }
    }
    if (p.type == SnapshotParamType::Enum) {
        if (r.options("options", &p.options, true) && p.options.empty()) r.fail(ErrorCode::Required, r.at("options"));
    } else if (!r.get("options", false).isUnbound()) {
        r.fail(ErrorCode::UnknownField, r.at("options"));
    }
    if (!r.ok()) return false;
    *out = std::move(p);
    return true;
}

}  // namespace

void encodeSnapshot(const CapabilitySnapshot &s, JsonObject out) {
    out["platform"] = s.platform;
    out["supported_features"] = s.supportedFeatures;
    putList(out, "source_list", s.sourceList);
    putList(out, "sound_mode_list", s.soundModeList);
    putList(out, "supported_color_modes", s.supportedColorModes);
    if (s.minColorTempKelvin > 0) out["min_color_temp_kelvin"] = s.minColorTempKelvin;
    if (s.maxColorTempKelvin > 0) out["max_color_temp_kelvin"] = s.maxColorTempKelvin;
    JsonArray services = out["services"].to<JsonArray>();
    for (const auto &service : s.services) {
        JsonObject o = services.add<JsonObject>();
        o["service"] = service.service;
        if (!service.label.empty()) o["label"] = service.label;
        JsonArray fields = o["fields"].to<JsonArray>();
        for (const auto &field : service.fields) encodeParam(field, fields.add<JsonObject>());
    }
}

bool decodeSnapshot(JsonVariantConst json, CapabilitySnapshot *out, ValidationReport *report,
                    const std::string &path) {
    Reader r(json, report, path);
    if (!r.ok()) return false;
    r.only({"platform", "supported_features", "source_list", "sound_mode_list", "supported_color_modes",
            "min_color_temp_kelvin", "max_color_temp_kelvin", "services"});
    CapabilitySnapshot s;
    if (r.string("platform", &s.platform, 32, true) && !isPlatform(s.platform)) {
        r.fail(ErrorCode::InvalidId, r.at("platform"));
    }
    int64_t n = 0;
    if (r.integer("supported_features", 0, 0xFFFFFFFFLL, &n, true)) s.supportedFeatures = static_cast<uint32_t>(n);
    r.options("source_list", &s.sourceList);
    r.options("sound_mode_list", &s.soundModeList);
    r.options("supported_color_modes", &s.supportedColorModes);
    if (r.integer("min_color_temp_kelvin", 0, 100000, &n)) s.minColorTempKelvin = static_cast<int32_t>(n);
    if (r.integer("max_color_temp_kelvin", 0, 100000, &n)) s.maxColorTempKelvin = static_cast<int32_t>(n);

    JsonVariantConst services = r.get("services", true);
    if (!services.isUnbound()) {
        if (!services.is<JsonArrayConst>()) {
            r.fail(ErrorCode::WrongType, r.at("services"));
        } else if (services.size() > kMaxSnapshotServices) {
            r.fail(ErrorCode::TooLong, r.at("services"));
        } else {
            size_t i = 0;
            for (JsonVariantConst item : services.as<JsonArrayConst>()) {
                const std::string where = r.at("services") + "/" + std::to_string(i++);
                Reader sr(item, report, where);
                if (!sr.ok()) {
                    r.taint();
                    continue;
                }
                sr.only({"service", "label", "fields"});
                SnapshotService service;
                if (sr.string("service", &service.service, limits::kMaxCommandIdLength, true)) {
                    const size_t dot = service.service.find('.');
                    const bool shaped = dot != std::string::npos &&
                                        isHaName(std::string_view(service.service).substr(0, dot)) &&
                                        isHaName(std::string_view(service.service).substr(dot + 1));
                    // Only the platform's own services (capabilities.cpp re-checks).
                    if (!shaped || service.service.substr(0, dot) != s.platform) {
                        sr.fail(ErrorCode::InvalidCommandId, sr.at("service"));
                    }
                }
                sr.string("label", &service.label, kMaxSnapshotLabelLength);
                JsonVariantConst fields = sr.get("fields", true);
                if (!fields.isUnbound()) {
                    if (!fields.is<JsonArrayConst>()) {
                        sr.fail(ErrorCode::WrongType, sr.at("fields"));
                    } else if (fields.size() > limits::kMaxParams) {
                        sr.fail(ErrorCode::TooManyParams, sr.at("fields"));
                    } else {
                        size_t f = 0;
                        for (JsonVariantConst field : fields.as<JsonArrayConst>()) {
                            SnapshotParam param;
                            const std::string fieldPath = sr.at("fields") + "/" + std::to_string(f++);
                            if (!decodeParam(field, &param, report, fieldPath)) {
                                sr.taint();
                                continue;
                            }
                            for (const auto &seen : service.fields) {
                                if (seen.name == param.name) sr.fail(ErrorCode::DuplicateParam, fieldPath + "/name");
                            }
                            service.fields.push_back(std::move(param));
                        }
                    }
                }
                if (!sr.ok()) {
                    r.taint();
                    continue;
                }
                s.services.push_back(std::move(service));
            }
        }
    }
    if (!r.ok()) return false;
    *out = std::move(s);
    return true;
}

}  // namespace hubbo::ha
