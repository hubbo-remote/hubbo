#include "hubbo/ha/services.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cmath>
#include <optional>

#include "hubbo/domain/limits.h"
#include "hubbo/ha/capabilities.h"
#include "hubbo/ha/snapshot_json.h"

namespace hubbo::ha {

namespace {

constexpr uint8_t kNesting = 16;

std::string bounded(JsonVariantConst v, size_t max) {
    if (!v.is<const char *>()) return {};
    std::string text = v.as<const char *>();
    if (text.size() > max) text.resize(max);
    return text;
}

// Feature masks as validated by HA: a list of ints (one may be a list of
// alternatives). False if something else is there.
bool readFeatures(JsonVariantConst v, std::vector<uint32_t> *out) {
    if (v.isNull()) return true;
    JsonArrayConst list = v.as<JsonArrayConst>();
    if (list.isNull()) {
        if (!v.is<uint32_t>()) return false;
        out->push_back(v.as<uint32_t>());
        return true;
    }
    for (JsonVariantConst item : list) {
        if (!item.is<uint32_t>()) return false;
        out->push_back(item.as<uint32_t>());
    }
    return true;
}

bool featuresMatch(const std::vector<uint32_t> &masks, uint32_t features) {
    if (masks.empty()) return true;
    for (uint32_t mask : masks) {
        if ((features & mask) == mask) return true;
    }
    return false;
}

std::vector<std::string> stringList(JsonVariantConst v) {
    std::vector<std::string> out;
    if (v.is<const char *>()) {
        out.emplace_back(v.as<const char *>());
    } else {
        for (JsonVariantConst item : v.as<JsonArrayConst>()) {
            if (item.is<const char *>()) out.emplace_back(item.as<const char *>());
        }
    }
    return out;
}

ServiceDirectory::EntityFilter readEntityFilter(JsonObjectConst f) {
    ServiceDirectory::EntityFilter filter;
    filter.integration = bounded(f["integration"], 64);
    filter.domains = stringList(f["domain"]);
    filter.unknownFeature = !readFeatures(f["supported_features"], &filter.features);
    return filter;
}

// HA's units, shortened where there's a common short form. A unit that
// still doesn't fit is dropped (never cut).
std::string shortUnit(JsonVariantConst v) {
    if (!v.is<const char *>()) return {};
    const std::string_view unit = v.as<const char *>();
    static constexpr struct {
        const char *from, *to;
    } kShort[] = {{"seconds", "s"}, {"second", "s"}, {"milliseconds", "ms"}, {"minutes", "min"},
                  {"minute", "min"}, {"hours", "h"},  {"hour", "h"},          {"percent", "%"}};
    for (const auto &s : kShort) {
        if (unit == s.from) return s.to;
    }
    return unit.size() <= kMaxSnapshotUnitLength ? std::string(unit) : std::string();
}

// HA's description as a short tooltip: whitespace runs become one space,
// and anything over kMaxSnapshotDescriptionLength bytes is cut at a word
// boundary (else a UTF-8 boundary) with an ellipsis. "" if it isn't a string.
std::string tooltip(JsonVariantConst v) {
    if (!v.is<const char *>()) return {};
    std::string out;
    for (const char *c = v.as<const char *>(); *c != '\0'; c++) {
        const bool space = *c == ' ' || *c == '\t' || *c == '\n' || *c == '\r';
        if (space) {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else {
            out += *c;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() <= kMaxSnapshotDescriptionLength) return out;
    static constexpr std::string_view kEllipsis = "\xE2\x80\xA6";  // U+2026
    size_t cut = kMaxSnapshotDescriptionLength - kEllipsis.size();
    const size_t space = out.rfind(' ', cut);
    if (space != std::string::npos && space > 0) {
        cut = space;
    } else {
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) cut--;  // a continuation byte
    }
    out.resize(cut);
    while (!out.empty() && (out.back() == ' ' || out.back() == ',' || out.back() == '.')) out.pop_back();
    return out + std::string(kEllipsis);
}

// "set_sleep_timer" -> "Set sleep timer": the label when HA gives none.
std::string readable(std::string_view id) {
    std::string out(id.substr(0, kMaxSnapshotLabelLength));
    for (char &c : out) {
        if (c == '_') c = ' ';
    }
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') out[0] = static_cast<char>(out[0] - 'a' + 'A');
    return out;
}

// Integral and within int32.
bool isInt(double v) {
    return std::isfinite(v) && std::floor(v) == v && v >= -2147483648.0 && v <= 2147483647.0;
}

// One field's selector -> typed param. False: unsupported (skips the service).
bool readSelector(JsonObjectConst selector, SnapshotParam *param, size_t *dropped) {
    if (selector.size() != 1) return false;
    const JsonPairConst only = *selector.begin();
    const std::string_view kind = only.key().c_str();
    JsonVariantConst config = only.value();
    if (kind == "boolean") {
        param->type = SnapshotParamType::Bool;
        return true;
    }
    if (kind == "number") {
        JsonVariantConst lo = config["min"];
        JsonVariantConst hi = config["max"];
        if (!lo.is<double>() || !hi.is<double>()) return false;
        param->minimum = lo.as<double>();
        param->maximum = hi.as<double>();
        if (!std::isfinite(param->minimum) || !std::isfinite(param->maximum) || param->minimum > param->maximum) {
            return false;
        }
        JsonVariantConst step = config["step"];
        double s = 1;
        bool any = false;
        if (step.is<double>()) {
            s = step.as<double>();
            if (!(s > 0) || !std::isfinite(s)) return false;
        } else if (step.is<const char *>() && std::string_view(step.as<const char *>()) == "any") {
            any = true;
        } else if (!step.isNull()) {
            return false;
        }
        const bool integral = !any && isInt(s) && isInt(param->minimum) && isInt(param->maximum);
        param->type = integral ? SnapshotParamType::Int : SnapshotParamType::Number;
        param->step = any ? 0 : s;
        param->unit = shortUnit(config["unit_of_measurement"]);
        return true;
    }
    if (kind == "select") {
        if (config["multiple"] | false) return false;
        std::vector<std::string> options;
        for (JsonVariantConst option : config["options"].as<JsonArrayConst>()) {
            if (option.is<const char *>()) {
                options.emplace_back(option.as<const char *>());
            } else if (option["value"].is<const char *>()) {
                options.emplace_back(option["value"].as<const char *>());
            } else {
                return false;
            }
        }
        if (!filterEnumOptions(options, dropped)) return false;
        param->type = SnapshotParamType::Enum;
        param->options = std::move(options);
        return true;
    }
    return false;  // text (no string params yet), entity, object, time, ...
}

void readFields(JsonObjectConst fields, ServiceDirectory::Service *service) {
    for (JsonPairConst pair : fields) {
        JsonObjectConst field = pair.value().as<JsonObjectConst>();
        // A section ({collapsed, fields}) groups fields for the UI: flatten.
        if (field["selector"].isNull() && field["fields"].is<JsonObjectConst>()) {
            readFields(field["fields"].as<JsonObjectConst>(), service);
            continue;
        }
        ServiceDirectory::Field f;
        f.param.name = pair.key().c_str();
        f.param.label = bounded(field["name"], kMaxSnapshotLabelLength);
        f.param.description = tooltip(field["description"]);
        f.param.required = field["required"] | false;
        JsonObjectConst filter = field["filter"].as<JsonObjectConst>();
        for (JsonPairConst key : filter) {
            if (std::string_view(key.key().c_str()) != "supported_features") f.unknownFilter = true;
        }
        if (!readFeatures(filter["supported_features"], &f.features)) f.unknownFilter = true;
        f.supported = isHaName(f.param.name) && !isForbiddenParamName(f.param.name) &&
                      readSelector(field["selector"].as<JsonObjectConst>(), &f.param, &service->droppedOptions);
        if (f.supported) applyDefault(field["default"], &f.param);  // a default that doesn't fit is dropped
        service->fields.push_back(std::move(f));
    }
}

// Labels from frontend/get_translations' resources (flat keys), over the
// description's own names.
void applyTranslations(std::string_view json, std::vector<ServiceDirectory::Service> *services) {
    if (json.empty() || services->empty()) return;
    JsonDocument doc;
    if (deserializeJson(doc, json.data(), json.size(), DeserializationOption::NestingLimit(2))) return;
    JsonObjectConst resources = doc.as<JsonObjectConst>();
    // A translation replaces the description's own text only when it has one.
    const auto take = [](std::string text, std::string *out) {
        if (!text.empty()) *out = std::move(text);
    };
    const auto name = [&](const std::string &key, std::string *out) {
        take(bounded(resources[key], kMaxSnapshotLabelLength), out);
    };
    for (ServiceDirectory::Service &service : *services) {
        const std::string base = "component." + service.domain + ".services." + service.service;
        name(base + ".name", &service.label);
        for (ServiceDirectory::Field &field : service.fields) {
            const std::string key = base + ".fields." + field.param.name;
            name(key + ".name", &field.param.label);
            take(tooltip(resources[key + ".description"]), &field.param.description);
        }
    }
}

}  // namespace

const char *ServiceDirectory::skipReasonName(SkipReason reason) {
    switch (reason) {
        case SkipReason::UnsupportedField: return "unsupported_field";
        case SkipReason::TooManyFields: return "too_many_fields";
        case SkipReason::ResponseOnly: return "response_only";
        case SkipReason::TooManyServices: return "too_many_services";
    }
    return "unsupported_field";
}

ServiceDirectory ServiceDirectory::parse(std::string_view json, const std::vector<std::string> &platforms,
                                         std::string_view translations) {
    ServiceDirectory directory;
    JsonDocument filter;
    for (const auto &platform : platforms) {
        if (isHaName(platform)) filter[platform] = true;
    }
    if (filter.isNull()) return directory;
    JsonDocument doc;
    if (deserializeJson(doc, json.data(), json.size(), DeserializationOption::Filter(filter),
                        DeserializationOption::NestingLimit(kNesting))) {
        return directory;
    }
    for (JsonPairConst domain : doc.as<JsonObjectConst>()) {
        for (JsonPairConst entry : domain.value().as<JsonObjectConst>()) {
            JsonObjectConst description = entry.value().as<JsonObjectConst>();
            Service service;
            service.domain = domain.key().c_str();
            service.service = entry.key().c_str();
            service.nameValid = isHaName(service.domain) && isHaName(service.service);
            service.label = bounded(description["name"], kMaxSnapshotLabelLength);
            service.responseOnly = description["response"].is<JsonObjectConst>() &&
                                   description["response"]["optional"] == false;
            JsonVariantConst entity = description["target"]["entity"];
            if (entity.is<JsonObjectConst>()) {
                service.targets.push_back(readEntityFilter(entity.as<JsonObjectConst>()));
            } else {
                for (JsonVariantConst f : entity.as<JsonArrayConst>()) {
                    if (f.is<JsonObjectConst>()) service.targets.push_back(readEntityFilter(f.as<JsonObjectConst>()));
                }
            }
            readFields(description["fields"].as<JsonObjectConst>(), &service);
            directory.services_.push_back(std::move(service));
        }
    }
    std::sort(directory.services_.begin(), directory.services_.end(), [](const Service &a, const Service &b) {
        return a.domain != b.domain ? a.domain < b.domain : a.service < b.service;
    });
    applyTranslations(translations, &directory.services_);
    for (Service &service : directory.services_) {
        if (service.label.empty()) service.label = readable(service.service);
        for (Field &field : service.fields) {
            if (field.param.label.empty()) field.param.label = readable(field.param.name);
        }
    }
    return directory;
}

ServiceDirectory::Offer ServiceDirectory::offerFor(std::string_view entityId, std::string_view platform,
                                                   uint32_t supportedFeatures) const {
    Offer offer;
    const size_t dot = entityId.find('.');
    if (dot == std::string_view::npos || platform.empty()) return offer;
    const std::string_view domain = entityId.substr(0, dot);
    if (platform == domain) return offer;  // the domain's own services are the curated allowlist

    // services_ is sorted by domain: only the platform's own run.
    const auto first = std::lower_bound(services_.begin(), services_.end(), platform,
                                        [](const Service &s, std::string_view p) { return s.domain < p; });
    for (auto it = first; it != services_.end() && it->domain == platform; ++it) {
        const Service &service = *it;
        bool targeted = false;
        for (const EntityFilter &f : service.targets) {
            const bool integration = f.integration.empty() || f.integration == platform;
            const bool inDomain =
                f.domains.empty() || std::find(f.domains.begin(), f.domains.end(), domain) != f.domains.end();
            targeted = targeted ||
                       (integration && inDomain && !f.unknownFeature && featuresMatch(f.features, supportedFeatures));
        }
        if (!targeted) continue;  // not for this entity (or no entity target at all)

        const std::string id = service.domain + "." + service.service;
        SnapshotService offered;
        offered.service = id;
        offered.label = service.label;
        bool usable = service.nameValid;
        for (const Field &field : service.fields) {
            if (field.unknownFilter) {
                usable = false;
                continue;
            }
            if (!featuresMatch(field.features, supportedFeatures)) continue;  // not for this entity
            usable = usable && field.supported;
            offered.fields.push_back(field.param);
        }
        // The first reason that applies, in this order; none: offered.
        const std::optional<SkipReason> reason =
            service.responseOnly                              ? SkipReason::ResponseOnly
            : !usable                                         ? SkipReason::UnsupportedField
            : offered.fields.size() > limits::kMaxParams      ? SkipReason::TooManyFields
            : offer.services.size() >= kMaxSnapshotServices   ? std::optional<SkipReason>(SkipReason::TooManyServices)
                                                              : std::nullopt;
        if (reason) {
            if (offer.skipped.size() < kMaxSnapshotServices) offer.skipped.push_back({id, *reason});
            continue;
        }
        offer.droppedOptions += service.droppedOptions;
        offer.services.push_back(std::move(offered));
    }
    return offer;
}

}  // namespace hubbo::ha
